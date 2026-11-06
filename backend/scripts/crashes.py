"""Read the crash reports testers' launchers sent to the Xerces backend. Standard library only.

    set XERCES_BACKEND_URL=https://<your-domain>
    set XERCES_ADMIN_TOKEN=<the value set on Railway>

    python scripts/crashes.py groups [--since 7d] [--limit 50]
    python scripts/crashes.py list [--since 7d] [--signature SIG] [--version V] [--limit 50] [--all]
    python scripts/crashes.py show ID [--json]
    python scripts/crashes.py minidump ID [-o FILE] [--force]

`groups` puts reports with the same signature together, the most frequent first. `list` shows
the newest reports first; `--all` follows every page. `show` prints one report in full, logs
included. `minidump` saves its stacks-only minidump, which WinDbg or Visual Studio opens (point
the symbol path at the release PDBs). `--since` takes 30m, 12h, 7d or an ISO date or time (UTC).

The token is sent only over HTTPS, or plain HTTP to this PC (127.0.0.1, localhost) for local
tests, and only to XERCES_BACKEND_URL: redirects are not followed. Anyone can send a crash report,
so control characters in one (terminal escape sequences) are printed as escapes, never as they are.
Exit codes: 0 done, 1 the server said no or could not be reached, 2 bad usage or setup.
"""

import argparse
import io
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Mapping
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any, TextIO

# (status, headers, body) for a GET of `url` with `headers`; raises OSError when unreachable.
Fetch = Callable[[str, Mapping[str, str]], tuple[int, Mapping[str, str], bytes]]

TIMEOUT = 30
LOCAL_HOSTS = frozenset({"localhost", "127.0.0.1", "::1"})
_RELATIVE = re.compile(r"(\d+)([mhd])")

# Control characters (C0, DEL, C1) and bidirectional overrides. ESC starts terminal escape
# sequences, which can clear the screen, fake text or (OSC 52, in Windows Terminal among others)
# set the clipboard.
_BIDI = r"\u061c\u200e\u200f\u202a-\u202e\u2066-\u2069"
_UNSAFE_LINE = re.compile(r"[\x00-\x1f\x7f-\x9f" + _BIDI + "]")
# The same, but tabs and line breaks stay (log tails).
_UNSAFE_TEXT = re.compile(r"[\x00-\x08\x0b-\x1f\x7f-\x9f" + _BIDI + "]")
# What json.dumps(ensure_ascii=False) leaves as it is (it escapes C0 controls itself).
_UNSAFE_JSON = re.compile(r"[\x7f-\x9f" + _BIDI + "]")


class CliError(Exception):
    def __init__(self, message: str, code: int = 1) -> None:
        super().__init__(message)
        self.code = code


def _escape(match: re.Match[str]) -> str:
    code = ord(match.group())
    return f"\\x{code:02x}" if code < 0x100 else f"\\u{code:04x}"


def line(value: Any) -> str:
    """A value from the server, for one line of output: control characters shown as escapes."""
    return _UNSAFE_LINE.sub(_escape, str(value))


def text_block(value: str) -> str:
    """A log tail from the server: control characters escaped, tabs and line breaks kept."""
    return _UNSAFE_TEXT.sub(_escape, value.replace("\r\n", "\n"))


def json_text(value: Any) -> str:
    """JSON for the terminal, still valid JSON: what json.dumps leaves raw becomes \\u escapes."""
    text = json.dumps(value, indent=2, ensure_ascii=False)
    return _UNSAFE_JSON.sub(lambda match: f"\\u{ord(match.group()):04x}", text)


class SafeText(io.TextIOBase):
    """Output that escapes control characters other than tabs and line breaks: the last guard,
    should a value from the server reach a print without line() or text_block()."""

    def __init__(self, stream: TextIO) -> None:
        super().__init__()
        self._stream = stream

    def write(self, text: str) -> int:
        self._stream.write(_UNSAFE_TEXT.sub(_escape, text))
        return len(text)

    def flush(self) -> None:
        self._stream.flush()


class _NoRedirects(urllib.request.HTTPRedirectHandler):
    """Refuse every redirect. urllib would send the Authorization header along to wherever one
    points, plain HTTP and other hosts included. The admin routes never redirect."""

    def redirect_request(self, *args: Any, **kwargs: Any) -> None:
        return None


_OPENER = urllib.request.build_opener(_NoRedirects)


def urllib_fetch(url: str, headers: Mapping[str, str]) -> tuple[int, Mapping[str, str], bytes]:
    request = urllib.request.Request(url, headers=dict(headers), method="GET")
    try:
        with _OPENER.open(request, timeout=TIMEOUT) as response:  # https checked, no redirects
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:  # also a refused redirect
        return error.code, dict(error.headers or {}), error.read()


class Backend:
    def __init__(self, base_url: str, token: str, fetch: Fetch) -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token
        self.fetch = fetch

    def get(
        self, path: str, params: Mapping[str, Any] | None = None
    ) -> tuple[Mapping[str, str], bytes]:
        query = {key: value for key, value in (params or {}).items() if value is not None}
        url = self.base_url + path + ("?" + urllib.parse.urlencode(query) if query else "")
        headers = {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/json",
            "User-Agent": "xerces-crashes/1",
        }
        try:
            status, response_headers, body = self.fetch(url, headers)
        except OSError as error:
            raise CliError(f"Could not reach {self.base_url}: {error}") from None
        if status == 200:
            return response_headers, body
        raise CliError(_error_text(status, response_headers, body))

    def json(self, path: str, params: Mapping[str, Any] | None = None) -> Any:
        _, body = self.get(path, params)
        return json.loads(body.decode("utf-8"))


def _error_text(status: int, headers: Mapping[str, str], body: bytes) -> str:
    message = ""
    try:
        message = line(json.loads(body.decode("utf-8"))["error"]["message"])
    except (ValueError, KeyError, TypeError, UnicodeDecodeError):
        pass
    lower = {key.lower(): value for key, value in headers.items()}
    if 300 <= status < 400:
        return (
            f"{status}: the server redirects to {line(lower.get('location', '?'))}. crashes.py "
            "does not follow redirects, so the token goes nowhere else; check XERCES_BACKEND_URL."
        )
    if status == 404 and message in ("", "Not Found"):
        return (
            "404: wrong URL, or XERCES_ADMIN_TOKEN is not set on the server (its admin routes "
            "answer 404 then)."
        )
    if status == 401:
        return "401: the server refused XERCES_ADMIN_TOKEN. Check that it matches Railway's."
    if status == 429:
        wait = line(lower.get("retry-after", "?"))
        return f"429: too many wrong tokens from this address. Try again in {wait} s."
    return f"{status}: {message or 'the server refused the request.'}"


def backend_from_env(env: Mapping[str, str], fetch: Fetch) -> Backend:
    base_url = env.get("XERCES_BACKEND_URL", "").strip()
    token = env.get("XERCES_ADMIN_TOKEN", "").strip()
    if not base_url:
        raise CliError("Set XERCES_BACKEND_URL to the backend's address (https://...).", 2)
    if not token:
        raise CliError("Set XERCES_ADMIN_TOKEN to the token set on the server.", 2)
    parts = urllib.parse.urlsplit(base_url)
    local = (parts.hostname or "").lower() in LOCAL_HOSTS
    if parts.scheme != "https" and not (parts.scheme == "http" and local):
        raise CliError(
            "XERCES_BACKEND_URL must start with https:// (http:// only for 127.0.0.1 or "
            "localhost), so the token never travels in the clear.",
            2,
        )
    return Backend(base_url, token, fetch)


def parse_since(value: str | None, now: datetime | None = None) -> str | None:
    if value is None:
        return None
    text = value.strip()
    relative = _RELATIVE.fullmatch(text.lower())
    if relative:
        amount, unit = int(relative.group(1)), relative.group(2)
        delta = {"m": timedelta(minutes=1), "h": timedelta(hours=1), "d": timedelta(days=1)}[unit]
        moment = (now or datetime.now(UTC)) - amount * delta
    else:
        try:
            moment = datetime.fromisoformat(text.replace("Z", "+00:00"))
        except ValueError:
            raise CliError(
                f"--since: use 30m, 12h, 7d or an ISO date or time, not {text!r}.", 2
            ) from None
        moment = moment.replace(tzinfo=UTC) if moment.tzinfo is None else moment.astimezone(UTC)
    return moment.isoformat(timespec="seconds").replace("+00:00", "Z")


def _when(value: str | None) -> str:
    if not value:
        return "-"
    try:
        moment = datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(UTC)
    except ValueError:
        return line(value)
    return moment.strftime("%Y-%m-%d %H:%M UTC")


def _or_dash(value: Any) -> str:
    return "-" if value is None or value == "" else line(value)


def _where(module: str | None, offset: str | None) -> str:
    if module and offset:
        return line(f"{module}+{offset}")
    return _or_dash(module or offset)


def cmd_groups(backend: Backend, args: argparse.Namespace, out: TextIO) -> int:
    since = parse_since(args.since)
    data = backend.json("/v1/admin/crash-groups", {"since": since, "limit": args.limit})
    groups = data["groups"]
    since = f" since {since}" if since else ""
    print(f"{data['reports']} reports in {len(groups)} groups{since}", file=out)
    for group in groups:
        print("", file=out)
        print(
            f"{group['count']:>5} reports  {group['installs']:>3} installs  "
            f"last {_when(group['last_seen'])}  first {_when(group['first_seen'])}",
            file=out,
        )
        where = _where(group["module"], group["offset"])
        print(f"  signature {line(group['signature'])}  {line(group['kind'])}  {where}", file=out)
        if group["frames"]:
            print(f"  frames    {', '.join(line(frame) for frame in group['frames'])}", file=out)
        versions = ", ".join(f"{line(v['value'])} ({v['count']})" for v in group["versions"])
        print(f"  versions  {versions}", file=out)
        if group["top_mods"]:
            mods = ", ".join(
                f"{_or_dash(m['name'] or m['id'])} ({m['count']})" for m in group["top_mods"]
            )
            print(f"  mods      {mods}", file=out)
        if group["top_maps"]:
            maps = ", ".join(f"{line(m['value'])} ({m['count']})" for m in group["top_maps"])
            print(f"  maps      {maps}", file=out)
        print(f"  sample    {line(group['sample_id'])}", file=out)
    return 0


def cmd_list(backend: Backend, args: argparse.Namespace, out: TextIO) -> int:
    params: dict[str, Any] = {
        "since": parse_since(args.since),
        "signature": args.signature,
        "version": args.version,
        "limit": args.limit,
    }
    print(
        f"{'RECEIVED':<20}  {'ID':<36}  {'KIND':<13}  {'CODE':<10}  {'PHASE':<12}  "
        f"{'VERSION':<16}  DUMP  WHERE",
        file=out,
    )
    shown = 0
    while True:
        page = backend.json("/v1/admin/crashes", params)
        for item in page["items"]:
            shown += 1
            print(
                f"{_when(item['received_at']):<20}  {line(item['id']):<36}  "
                f"{line(item['kind']):<13}  {_or_dash(item['code']):<10}  "
                f"{_or_dash(item['phase']):<12}  {line(item['version']):<16}  "
                f"{'yes' if item['has_minidump'] else 'no':<4}  "
                f"{_where(item['module'], item['offset'])}",
                file=out,
            )
        if not args.all or not page["next_cursor"]:
            break
        params["cursor"] = page["next_cursor"]
    if not shown:
        print("(no reports)", file=out)
    elif page["next_cursor"]:
        print("(more: add --all)", file=out)
    return 0


def _section(title: str, out: TextIO) -> None:
    print(f"\n== {title}", file=out)


def cmd_show(backend: Backend, args: argparse.Namespace, out: TextIO) -> int:
    report = backend.json(f"/v1/admin/crashes/{urllib.parse.quote(args.id, safe='')}")
    if args.json:
        print(json_text(report), file=out)
        return 0
    crash = report.get("crash") or {}
    xerces = report.get("xerces") or {}
    game = report.get("game") or {}
    launch = report.get("launch") or {}
    system = report.get("system") or {}
    logs = report.get("logs") or {}
    print(f"Crash report {line(report['id'])}", file=out)
    made = _when(report["created_utc"])
    print(f"  received   {_when(report['received_at'])} (made {made})", file=out)
    print(f"  install    {line(report['install_id'])}", file=out)
    print(f"  signature  {line(report['signature'])}", file=out)
    dump = f"yes, {line(report['minidump_size'])} bytes" if report["has_minidump"] else "no"
    print(f"  minidump   {dump}", file=out)
    dirty = " (dirty)" if xerces.get("dirty") else ""
    commit = line(xerces.get("commit") or "")
    print(f"  xerces     {_or_dash(xerces.get('version'))} {commit}{dirty}".rstrip(), file=out)
    steam = _or_dash(game.get("steam_build"))
    print(f"  game       build {_or_dash(game.get('build'))}, Steam build {steam}", file=out)

    _section("Crash", out)
    for key in ("kind", "code", "exit_code", "module", "offset", "thread", "phase"):
        print(f"  {key:<10} {_or_dash(crash.get(key))}", file=out)
    for index, frame in enumerate(crash.get("frames") or []):
        print(f"  #{index:<3} {line(frame)}", file=out)

    _section("Launch", out)
    safe_mode = {True: "yes", False: "no"}.get(launch.get("safe_mode"), "-")
    print(
        f"  mode {_or_dash(launch.get('mode'))}, map {_or_dash(launch.get('map'))}, "
        f"plan {_or_dash(launch.get('plan_id'))}, safe mode {safe_mode}",
        file=out,
    )
    if launch.get("flags"):
        print(f"  flags {' '.join(line(flag) for flag in launch['flags'])}", file=out)
    for mod in launch.get("mods") or []:
        name, mod_id, version = (_or_dash(mod.get(key)) for key in ("name", "id", "version"))
        print(f"  mod  {name}  {mod_id}  {version}", file=out)

    _section("System", out)
    windows, display = _or_dash(system.get("windows")), _or_dash(system.get("display"))
    print(f"  Windows {windows}, display {display}", file=out)
    threads = line(system.get("logical_cpus") or "?")
    ram = line(system.get("ram_mib") or "?")
    print(f"  CPU {_or_dash(system.get('cpu'))} ({threads} threads), RAM {ram} MiB", file=out)
    for gpu in system.get("gpus") or []:
        name, driver = _or_dash(gpu.get("name")), _or_dash(gpu.get("driver"))
        print(f"  GPU {name}, driver {driver}", file=out)

    if logs.get("journal") is not None:
        _section("Journal", out)
        print(json_text(logs["journal"]), file=out)
    for key, title in (
        ("game_log_tail", "Game log (tail)"),
        ("console_tail", "Console (tail)"),
        ("launcher_log_tail", "Launcher log (tail)"),
    ):
        if logs.get(key):
            _section(title, out)
            print(text_block(logs[key]).rstrip("\n"), file=out)
    return 0


def cmd_minidump(backend: Backend, args: argparse.Namespace, out: TextIO) -> int:
    target = Path(args.output) if args.output else Path(f"{args.id}.dmp")
    if target.exists() and not args.force:
        raise CliError(f"{target} exists; add --force to replace it.", 2)
    _, body = backend.get(f"/v1/admin/crashes/{urllib.parse.quote(args.id, safe='')}/minidump")
    target.write_bytes(body)
    print(f"Saved {len(body)} bytes to {target}", file=out)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="crashes.py",
        description="Read crash reports from the Xerces backend "
        "(needs XERCES_BACKEND_URL and XERCES_ADMIN_TOKEN).",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    groups = commands.add_parser("groups", help="reports by signature, most frequent first")
    groups.add_argument("--since", help="30m, 12h, 7d or an ISO date or time")
    groups.add_argument("--limit", type=int, default=50, help="groups to show (at most 500)")
    groups.set_defaults(run=cmd_groups)

    listing = commands.add_parser("list", help="reports, newest first")
    listing.add_argument("--since", help="30m, 12h, 7d or an ISO date or time")
    listing.add_argument("--signature", help="only this signature (16 hex characters)")
    listing.add_argument("--version", help="only this Xerces version")
    listing.add_argument("--limit", type=int, default=50, help="reports per page (at most 200)")
    listing.add_argument("--all", action="store_true", help="follow every page")
    listing.set_defaults(run=cmd_list)

    show = commands.add_parser("show", help="one report in full")
    show.add_argument("id", help="the report's server id")
    show.add_argument("--json", action="store_true", help="print the raw JSON")
    show.set_defaults(run=cmd_show)

    minidump = commands.add_parser("minidump", help="save a report's minidump")
    minidump.add_argument("id", help="the report's server id")
    minidump.add_argument("-o", "--output", help="file to write (default: <id>.dmp here)")
    minidump.add_argument("--force", action="store_true", help="replace an existing file")
    minidump.set_defaults(run=cmd_minidump)
    return parser


def main(
    argv: list[str] | None = None,
    *,
    env: Mapping[str, str] | None = None,
    fetch: Fetch | None = None,
    out: TextIO | None = None,
    err: TextIO | None = None,
) -> int:
    out = out or sys.stdout
    err = err or sys.stderr
    for stream in (out, err):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(errors="replace")  # a console that cannot show a character
    args = build_parser().parse_args(argv)
    safe_out, safe_err = SafeText(out), SafeText(err)
    try:
        backend = backend_from_env(os.environ if env is None else env, fetch or urllib_fetch)
        return args.run(backend, args, safe_out)
    except CliError as error:
        print(f"crashes.py: {error}", file=safe_err)
        return error.code
    finally:
        safe_out.flush()
        safe_err.flush()


if __name__ == "__main__":
    sys.exit(main())
