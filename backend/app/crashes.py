"""Crash reports: decoding, validation, the minidump's privacy mask and the crash signature.

The launcher redacts a report before it sends it (profile paths, user names, server addresses).
The server checks again that no Windows profile path such as C:\\Users\\<name> survives: in the
text it becomes C:\\Users\\<user> (app.scrub, which the schema runs on each field it keeps), and
in the minidump, which is binary, the name's characters are overwritten in place so every offset
in the file stays valid. No client address is stored.

What a report costs is bounded before the deep work: the body is at most 4 MiB, everything but
the minidump at most 1 MiB (checked before parsing), validation drops unknown fields without
looking into them, and only the fields kept are scrubbed. Everything here is plain CPU work, run
in a worker thread (app.routers.crashes).
"""

import base64
import binascii
import hashlib
import json
import re
import zlib
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

from pydantic import ValidationError

from app.errors import ApiError
from app.schemas import CRASH_MINIDUMP_B64_MAX, CRASH_REST_MAX, CrashReportIn
from app.scrub import SHARED_PROFILES
from app.security import size_text

MAX_MINIDUMP_BYTES = 2 * 1024 * 1024
INSTALL_ID = re.compile(r"[0-9a-f]{32}")
MINIDUMP_SIGNATURE = b"MDMP"
# The minidump field as JSON text: base64, maybe with escaped line breaks or slashes. Wherever
# this matches in valid JSON the run is inside one JSON string, so parsing it costs its size.
_MINIDUMP_FIELD = re.compile(rb'"minidump_b64"\s*:\s*"([A-Za-z0-9+/=\\]*)')
# The largest minidump's base64, with room for escaped line breaks and slashes.
_MINIDUMP_FIELD_MAX = CRASH_MINIDUMP_B64_MAX * 9 // 8

# The profile prefixes inside a minidump: UTF-16LE (module paths) and ASCII or UTF-8 (CodeView
# records, which name where each module's PDB was built).
_UTF16_PROFILE = re.compile(
    rb"[A-Za-z]\x00:\x00(?:[\\/]\x00)+u\x00s\x00e\x00r\x00s\x00(?:[\\/]\x00)+", re.IGNORECASE
)
_BYTES_PROFILE = re.compile(rb"[A-Za-z]:[\\/]+users[\\/]+", re.IGNORECASE)
_BYTES_NAME_END = frozenset(b'\\/:*?"<>|') | frozenset(range(0x20))
_MAX_NAME_UNITS = 256
_HEX = re.compile(r"0x0*(?=[0-9a-f])")


def invalid(message: str) -> ApiError:
    return ApiError(400, "invalid_crash_report", message)


def check_install_id(value: str | None) -> str:
    if value is None or not INSTALL_ID.fullmatch(value):
        raise ApiError(
            400, "invalid_install_id", "X-Xerces-Install must be 32 lower-case hex characters."
        )
    return value


# ----------------------------------------------------------------------------- body


def decode_body(
    raw: bytes, content_type: str | None, content_encoding: str | None, limit: int
) -> bytes:
    """The JSON bytes of a request: checks the media type and undoes gzip, at most `limit` bytes.

    A wrong media type or encoding is 400 like any other invalid report: the contract's answers
    are 400 (drop it), 413 (too large) and 429 (later), and launchers drop only on 400 and 413.
    """
    media_type, _, parameters = (content_type or "").partition(";")
    if media_type.strip().lower() != "application/json":
        raise ApiError(400, "unsupported_media_type", "Content-Type must be application/json.")
    charset = re.search(r"charset\s*=\s*\"?([\w-]+)", parameters, re.IGNORECASE)
    if charset and charset.group(1).lower() not in ("utf-8", "utf8"):
        raise ApiError(400, "unsupported_media_type", "The body must be UTF-8.")

    encoding = (content_encoding or "").strip().lower()
    if encoding in ("", "identity"):
        if len(raw) > limit:
            raise too_large(limit)
        return raw
    if encoding not in ("gzip", "x-gzip"):
        raise ApiError(400, "unsupported_media_type", "Content-Encoding must be gzip or absent.")
    # One bounded step, so a small bomb never inflates past the limit in memory.
    decoder = zlib.decompressobj(wbits=16 + zlib.MAX_WBITS)
    try:
        body = decoder.decompress(raw, limit + 1)
    except zlib.error:
        raise invalid("The body is not valid gzip.") from None
    if len(body) > limit:
        raise too_large(limit)
    if not decoder.eof or decoder.unused_data:
        raise invalid("The body is not one complete gzip stream.")
    return body


def too_large(limit: int) -> ApiError:
    return ApiError(413, "payload_too_large", f"The report is larger than {size_text(limit)}.")


def size_without_minidump(body: bytes) -> int:
    """The body's size less its minidump field's value (the longest such run, up to the
    largest minidump's base64), found without parsing."""
    longest = max((len(match.group(1)) for match in _MINIDUMP_FIELD.finditer(body)), default=0)
    return len(body) - min(longest, _MINIDUMP_FIELD_MAX)


def _reject_constant(name: str) -> Any:
    raise ValueError(name)


def parse_json(body: bytes) -> dict[str, Any]:
    try:
        text = body.decode("utf-8")
        data = json.loads(text.removeprefix("\ufeff"), parse_constant=_reject_constant)
    except UnicodeDecodeError:
        raise invalid("The body is not UTF-8.") from None
    except ValueError:  # also NaN and Infinity, which no database stores as JSON
        raise invalid("The body is not valid JSON.") from None
    except RecursionError:
        raise invalid("The report is nested too deeply.") from None
    if not isinstance(data, dict):
        raise invalid("The report must be a JSON object.")
    return data


def _first_error(exc: ValidationError) -> str:
    # Only where and which rule: never the submitted value.
    errors = exc.errors(include_input=False, include_url=False)
    if not errors:
        return "The report is not valid."
    first = errors[0]
    location = ".".join(str(part) for part in first.get("loc", ()))
    message = str(first.get("msg", "Invalid value.")).removeprefix("Value error, ")
    return f"{location}: {message}" if location else message


# ----------------------------------------------------------------------------- minidump


def mask_minidump(data: bytes) -> bytes:
    """The minidump with every profile folder name overwritten by '_', keeping its length.

    Debuggers find modules and symbols by file name, size and timestamp, so the folders above a
    module do not matter to them. Release PDBs are found through the symbol path.
    """
    out = bytearray(data)
    for match in _UTF16_PROFILE.finditer(data):
        start = end = match.end()
        while end + 1 < len(out) and (end - start) // 2 < _MAX_NAME_UNITS:
            unit = out[end] | (out[end + 1] << 8)
            if unit < 0x80 and unit in _BYTES_NAME_END:
                break
            end += 2
        name = bytes(out[start:end]).decode("utf-16-le", "replace")
        if end > start and name.strip().lower() not in SHARED_PROFILES:
            out[start:end] = b"_\x00" * ((end - start) // 2)
    for match in _BYTES_PROFILE.finditer(data):
        start = end = match.end()
        while end < len(out) and end - start < _MAX_NAME_UNITS and out[end] not in _BYTES_NAME_END:
            end += 1
        name = bytes(out[start:end]).decode("utf-8", "replace")
        if end > start and name.strip().lower() not in SHARED_PROFILES:
            out[start:end] = b"_" * (end - start)
    return bytes(out)


def decode_minidump(value: str | None) -> bytes | None:
    if not value:
        return None
    text = "".join(value.split())  # line breaks some encoders add
    if len(text) > CRASH_MINIDUMP_B64_MAX:
        raise ApiError(413, "payload_too_large", "The minidump is larger than 2 MiB.")
    try:
        data = base64.b64decode(text, validate=True)
    except (binascii.Error, ValueError):
        raise invalid("minidump_b64 is not valid base64.") from None
    if len(data) > MAX_MINIDUMP_BYTES:
        raise ApiError(413, "payload_too_large", "The minidump is larger than 2 MiB.")
    if not data.startswith(MINIDUMP_SIGNATURE):
        raise invalid("minidump_b64 is not a minidump.")
    return mask_minidump(data)


# ----------------------------------------------------------------------------- signature


def crash_signature(
    kind: str, module: str | None, offset: str | None, frames: Sequence[str] | None
) -> str:
    """The group a crash belongs to: the first 16 hex characters of the SHA-256 of kind, module,
    offset and the first three frames, lower-cased, without leading zeros in hex numbers, joined
    with '|'. Missing parts count as empty."""
    parts = [kind, module or "", offset or "", *list(frames or ())[:3]]
    text = "|".join(_HEX.sub("0x", part.strip().lower()) for part in parts)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


# ----------------------------------------------------------------------------- whole report


@dataclass(frozen=True)
class PreparedReport:
    report_id: str
    columns: dict[str, Any]  # CrashReport fields, except id, install_id and received_at
    minidump: bytes | None

    @property
    def stored_bytes(self) -> int:
        return int(self.columns["stored_bytes"])


def stored_size(columns: dict[str, Any], minidump: bytes | None) -> int:
    """About what a report takes in the database: its strings as UTF-8, its JSON parts as the
    database driver writes them (non-ASCII as \\u escapes) and the minidump."""
    size = len(minidump or b"")
    for value in columns.values():
        if isinstance(value, str):
            size += len(value.encode("utf-8"))
        elif isinstance(value, dict | list):
            size += len(json.dumps(value))
    return size


def prepare_report(
    raw: bytes, content_type: str | None, content_encoding: str | None, limit: int
) -> PreparedReport:
    """Decode, validate and scrub a POST /v1/crashes body, or raise ApiError (400, 413)."""
    return prepare_body(decode_body(raw, content_type, content_encoding, limit))


def prepare_body(body: bytes) -> PreparedReport:
    """Validate and scrub a decoded body (decode_body), or raise ApiError (400, 413)."""
    if size_without_minidump(body) > CRASH_REST_MAX:
        raise ApiError(
            413,
            "payload_too_large",
            f"The report without its minidump is larger than {size_text(CRASH_REST_MAX)}.",
        )
    data = parse_json(body)
    try:
        # Checks every cap as sent, then scrubs each field it keeps (app.schemas).
        report = CrashReportIn.model_validate(data)
    except ValidationError as exc:
        raise invalid(_first_error(exc)) from None
    del data  # unknown fields go now, not at the end

    minidump = decode_minidump(report.minidump_b64)
    logs = report.logs.model_dump(mode="json") if report.logs else None
    crash = report.crash.model_dump(mode="json")
    game = report.game
    launch = report.launch
    system = report.system.model_dump(mode="json") if report.system else None
    mods = None
    if launch is not None and launch.mods is not None:
        mods = [mod.model_dump(mode="json") for mod in launch.mods]
    columns: dict[str, Any] = {
        "report_id": report.report_id,
        "created_utc": report.created_utc,
        "version": report.xerces.version,
        "commit": report.xerces.commit,
        "dirty": report.xerces.dirty,
        "game_build": game.build if game else None,
        "steam_build": game.steam_build if game else None,
        "kind": report.crash.kind,
        "code": report.crash.code,
        "module": report.crash.module,
        "offset": report.crash.offset,
        "signature": crash_signature(
            report.crash.kind, report.crash.module, report.crash.offset, report.crash.frames
        ),
        "phase": report.crash.phase,
        "mode": launch.mode if launch else None,
        "map": launch.map if launch else None,
        "plan_id": launch.plan_id if launch else None,
        "safe_mode": launch.safe_mode if launch else None,
        "flags": launch.flags if launch else None,
        "mods": mods,
        "system": system,
        "crash": crash,
        "logs": logs,
    }
    columns["stored_bytes"] = stored_size(columns, minidump)
    return PreparedReport(report_id=report.report_id, columns=columns, minidump=minidump)
