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
tests. Exit codes: 0 done, 1 the server said no or could not be reached, 2 bad usage or setup.
"""

import argparse
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


class CliError(Exception):
    def __init__(self, message: str, code: int = 1) -> None:
        super().__init__(message)
        self.code = code


def urllib_fetch(url: str, headers: Mapping[str, str]) -> tuple[int, Mapping[str, str], bytes]:
    request = urllib.request.Request(url, headers=dict(headers), method="GET")
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as response:  # noqa: S310 (checked)
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
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
        message = json.loads(body.decode("utf-8"))["error"]["message"]
    except (ValueError, KeyError, TypeError, UnicodeDecodeError):
        pass
    if status == 404 and message in ("", "Not Found"):
        return (
            "404: wrong URL, or XERCES_ADMIN_TOKEN is not set on the server (its admin routes "
            "answer 404 then)."
        )
    if status == 401:
        return "401: the server refused XERCES_ADMIN_TOKEN. Check that it matches Railway's."
    if status == 429:
        wait = {key.lower(): value for key, value in headers.items()}.get("retry-after", "?")
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
        return value
    return moment.strftime("%Y-%m-%d %H:%M UTC")


def _where(module: str | None, offset: str | None) -> str:
    if module and offset:
        return f"{module}+{offset}"
    return module or offset or "-"


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
        print(f"  signature {group['signature']}  {group['kind']}  {where}", file=out)
        if group["frames"]:
            print(f"  frames    {', '.join(group['frames'])}", file=out)
        versions = ", ".join(f"{v['value']} ({v['count']})" for v in group["versions"])
        print(f"  versions  {versions}", file=out)
        if group["top_mods"]:
            mods = ", ".join(f"{m['name'] or m['id']} ({m['count']})" for m in group["top_mods"])
            print(f"  mods      {mods}", file=out)
        if group["top_maps"]:
            maps = ", ".join(f"{m['value']} ({m['count']})" for m in group["top_maps"])
            print(f"  maps      {maps}", file=out)
        print(f"  sample    {group['sample_id']}", file=out)
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
                f"{_when(item['received_at']):<20}  {item['id']:<36}  {item['kind']:<13}  "
                f"{item['code'] or '-':<10}  {item['phase'] or '-':<12}  {item['version']:<16}  "
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
        print(json.dumps(report, indent=2, ensure_ascii=False), file=out)
        return 0
    crash = report.get("crash") or {}
    xerces = report.get("xerces") or {}
    game = report.get("game") or {}
    launch = report.get("launch") or {}
    system = report.get("system") or {}
    logs = report.get("logs") or {}
    print(f"Crash report {report['id']}", file=out)
    made = _when(report["created_utc"])
    print(f"  received   {_when(report['received_at'])} (made {made})", file=out)
    print(f"  install    {report['install_id']}", file=out)
    print(f"  signature  {report['signature']}", file=out)
    dump = f"yes, {report['minidump_size']} bytes" if report["has_minidump"] else "no"
    print(f"  minidump   {dump}", file=out)
    dirty = " (dirty)" if xerces.get("dirty") else ""
    commit = xerces.get("commit") or ""
    print(f"  xerces     {xerces.get('version')} {commit}{dirty}".rstrip(), file=out)
    steam = game.get("steam_build") or "-"
    print(f"  game       build {game.get('build') or '-'}, Steam build {steam}", file=out)

    _section("Crash", out)
    for key in ("kind", "code", "exit_code", "module", "offset", "thread", "phase"):
        print(f"  {key:<10} {crash.get(key) if crash.get(key) is not None else '-'}", file=out)
    for index, frame in enumerate(crash.get("frames") or []):
        print(f"  #{index:<3} {frame}", file=out)

    _section("Launch", out)
    safe_mode = {True: "yes", False: "no"}.get(launch.get("safe_mode"), "-")
    print(
        f"  mode {launch.get('mode') or '-'}, map {launch.get('map') or '-'}, "
        f"plan {launch.get('plan_id') or '-'}, safe mode {safe_mode}",
        file=out,
    )
    if launch.get("flags"):
        print(f"  flags {' '.join(launch['flags'])}", file=out)
    for mod in launch.get("mods") or []:
        name, mod_id, version = (mod.get(key) or "-" for key in ("name", "id", "version"))
        print(f"  mod  {name}  {mod_id}  {version}", file=out)

    _section("System", out)
    windows, display = system.get("windows") or "-", system.get("display") or "-"
    print(f"  Windows {windows}, display {display}", file=out)
    threads, ram = system.get("logical_cpus") or "?", system.get("ram_mib") or "?"
    print(f"  CPU {system.get('cpu') or '-'} ({threads} threads), RAM {ram} MiB", file=out)
    for gpu in system.get("gpus") or []:
        print(f"  GPU {gpu.get('name') or '-'}, driver {gpu.get('driver') or '-'}", file=out)

    if logs.get("journal") is not None:
        _section("Journal", out)
        print(json.dumps(logs["journal"], indent=2, ensure_ascii=False), file=out)
    for key, title in (
        ("game_log_tail", "Game log (tail)"),
        ("console_tail", "Console (tail)"),
        ("launcher_log_tail", "Launcher log (tail)"),
    ):
        if logs.get(key):
            _section(title, out)
            print(logs[key].rstrip("\n"), file=out)
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
    try:
        backend = backend_from_env(os.environ if env is None else env, fetch or urllib_fetch)
        return args.run(backend, args, out)
    except CliError as error:
        print(f"crashes.py: {error}", file=err)
        return error.code


if __name__ == "__main__":
    sys.exit(main())
