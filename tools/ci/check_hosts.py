"""Fails when code names a network host that is not reviewed (docs/ENDPOINTS.md).

usage: python tools/ci/check_hosts.py

Scans src/ and data/ (C++, Lua, HTML, JS) for URLs and quoted host names and
compares them with tools/ci/hosts.json. A listed host also covers its
subdomains, so "demonware.net" covers "stun.us.demonware.net".
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXTENSIONS = {".cpp", ".hpp", ".h", ".c", ".lua", ".html", ".js"}
URL = re.compile(r"\b[a-z][a-z0-9+.-]*://([A-Za-z0-9.-]+\.[A-Za-z]{2,})")
QUOTED = re.compile(r"[\"']([a-z0-9-]+(?:\.[a-z0-9-]+)*\.(?:com|net|org|lol|gg|io|dev|app|cloud|co|me|tv))(?::\d+)?[\"']")


# Names reserved for documentation and tests (RFC 2606, RFC 6761): nothing
# answers on them, so tests may use them freely.
RESERVED = ["example.com", "example.net", "example.org", "example", "test", "invalid", "localhost"]


def allowed(host: str, hosts: list[str]) -> bool:
    host = host.lower().rstrip(".")
    return any(host == h or host.endswith("." + h) for h in hosts + RESERVED)


def main() -> int:
    hosts = json.loads((ROOT / "tools" / "ci" / "hosts.json").read_text(encoding="utf-8"))["hosts"]
    problems = []
    for folder in ("src", "data"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.suffix.lower() not in EXTENSIONS or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if path.suffix.lower() in {".cpp", ".hpp", ".h", ".c"}:
                # Comments name hosts for documentation; they contact nothing.
                # Blank them but keep line numbers.
                text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group().count("\n"), text, flags=re.S)
                text = re.sub(r"(?<![:\"'])//[^\n]*", "", text)
            for number, line in enumerate(text.splitlines(), 1):
                for host in URL.findall(line) + QUOTED.findall(line):
                    if not allowed(host, hosts):
                        problems.append(f"{path.relative_to(ROOT).as_posix()}:{number}: {host}")
    for problem in problems:
        print(f"unreviewed host: {problem}")
    print(f"hosts: {'FAIL' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
