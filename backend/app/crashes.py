"""Crash reports: decoding, validation, the server's own privacy scrub and the crash signature.

The launcher redacts a report before it sends it (profile paths, user names, server addresses).
The server checks again that no Windows profile path such as C:\\Users\\<name> survives: in the
text it becomes C:\\Users\\<user>, and in the minidump, which is binary, the name's characters are
overwritten in place so every offset in the file stays valid. No client address is stored.

Everything here is plain CPU work, run in a worker thread (app.routers.crashes).
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
from app.schemas import CRASH_MINIDUMP_B64_MAX, CrashReportIn
from app.security import size_text

MAX_MINIDUMP_BYTES = 2 * 1024 * 1024
MAX_JOURNAL_BYTES = 256 * 1024
MAX_DEPTH = 32
INSTALL_ID = re.compile(r"[0-9a-f]{32}")
USER_PLACEHOLDER = "<user>"
MINIDUMP_SIGNATURE = b"MDMP"
# Profile folders that name nobody; kept so paths into them stay readable.
SHARED_PROFILES = frozenset({"public", "default", "default user", "all users"})

# A drive, then Users (or the old Documents and Settings), then the profile folder's name: every
# character up to the next path separator, quote or control character. JSON-escaped backslashes
# (C:\\Users\\name) match too. Over-matching only hides more.
_NAME_END = r"\\/:*?\"<>|\x00-\x1f"
_PROFILE_PATH = re.compile(
    r"([A-Za-z]:[\\/]+(?:Users|Documents and Settings)[\\/]+)([^" + _NAME_END + r"]+)",
    re.IGNORECASE,
)
# Proton and Linux: /home/<name> or Z:\home\<name>, where a path starts (not ui/home/menu). A
# match may start only at the first slash of a run, so a long run of slashes costs linear time.
_HOME_PATH = re.compile(
    r"((?<![\w.~\\/-])(?:[A-Za-z]:)?[\\/]+home[\\/]+)([^" + _NAME_END + r"]+)",
    re.IGNORECASE,
)
# The same prefixes inside a minidump: UTF-16LE (module paths) and ASCII or UTF-8 (CodeView
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
    """The JSON bytes of a request: checks the media type and undoes gzip, at most `limit` bytes."""
    media_type, _, parameters = (content_type or "").partition(";")
    if media_type.strip().lower() != "application/json":
        raise ApiError(415, "unsupported_media_type", "Content-Type must be application/json.")
    charset = re.search(r"charset\s*=\s*\"?([\w-]+)", parameters, re.IGNORECASE)
    if charset and charset.group(1).lower() not in ("utf-8", "utf8"):
        raise ApiError(415, "unsupported_media_type", "The body must be UTF-8.")

    encoding = (content_encoding or "").strip().lower()
    if encoding in ("", "identity"):
        if len(raw) > limit:
            raise too_large(limit)
        return raw
    if encoding not in ("gzip", "x-gzip"):
        raise ApiError(415, "unsupported_media_type", "Content-Encoding must be gzip or absent.")
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


# ----------------------------------------------------------------------------- privacy


def _replace_name(match: re.Match[str]) -> str:
    name = match.group(2)
    if name.strip().lower() in SHARED_PROFILES:
        return match.group(0)
    return match.group(1) + USER_PLACEHOLDER


def scrub_text(text: str) -> str:
    """Profile paths with the user name replaced, NULs removed and lone surrogates replaced."""
    if "\x00" in text:
        text = text.replace("\x00", "")
    try:
        text.encode("utf-8")
    except UnicodeEncodeError:  # a lone surrogate ("\ud800") parses as JSON but is not text
        text = text.encode("utf-8", "replace").decode("utf-8")
    if ":" in text:
        text = _PROFILE_PATH.sub(_replace_name, text)
    return _HOME_PATH.sub(_replace_name, text)


def scrub(value: Any) -> Any:
    """A copy of a parsed JSON value with every string (keys too) scrubbed. Iterative, and
    refuses nesting deeper than MAX_DEPTH, so no input can exhaust the stack."""
    root: list[Any] = [None]
    stack: list[tuple[Any, list[Any] | dict[str, Any], Any, int]] = [(value, root, 0, 0)]
    while stack:
        node, parent, slot, depth = stack.pop()
        if depth > MAX_DEPTH:
            raise invalid("The report is nested too deeply.")
        if isinstance(node, dict):
            copy: dict[str, Any] = {}
            parent[slot] = copy
            for key, item in node.items():
                clean_key = scrub_text(key)
                copy[clean_key] = None
                stack.append((item, copy, clean_key, depth + 1))
        elif isinstance(node, list):
            items: list[Any] = [None] * len(node)
            parent[slot] = items
            stack.extend((item, items, index, depth + 1) for index, item in enumerate(node))
        elif isinstance(node, str):
            parent[slot] = scrub_text(node)
        else:
            parent[slot] = node
    return root[0]


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


def prepare_report(
    raw: bytes, content_type: str | None, content_encoding: str | None, limit: int
) -> PreparedReport:
    """Decode, scrub and validate a POST /v1/crashes body, or raise ApiError (400, 413, 415)."""
    data = parse_json(decode_body(raw, content_type, content_encoding, limit))
    # The minidump is masked as bytes below; base64 text can hold "/home/" by chance.
    minidump_b64 = data.pop("minidump_b64", None)
    data = scrub(data)
    data["minidump_b64"] = minidump_b64
    try:
        report = CrashReportIn.model_validate(data)
    except ValidationError as exc:
        raise invalid(_first_error(exc)) from None

    minidump = decode_minidump(report.minidump_b64)
    logs = report.logs.model_dump(mode="json") if report.logs else None
    if logs is not None and logs["journal"] is not None:
        size = len(json.dumps(logs["journal"], ensure_ascii=False, separators=(",", ":")).encode())
        if size > MAX_JOURNAL_BYTES:
            raise invalid(f"logs.journal is larger than {MAX_JOURNAL_BYTES // 1024} KB.")

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
    return PreparedReport(report_id=report.report_id, columns=columns, minidump=minidump)
