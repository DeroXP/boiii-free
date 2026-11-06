"""Fails when upstream's updater or the -beta self-download could run again (CLAUDE.md hard rule).

usage: python tools/ci/check_switches.py

62e28935 turned both off behind constexpr switches, so upstream merges stay
small. Upstream's updater replaces a renamed build with upstream's boiii.exe
and deletes files under %LOCALAPPDATA%\\boiii it does not know; -beta
downloaded boiii-beta.exe from upstream's server and ran it. For each one:
while its code is in the file, its switch must be there and false. Code that
was removed (as D9 may do) needs no switch.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

# file, switch, a marker of the code the switch keeps from running
SWITCHES = [
    ("src/client/component/updater.cpp", "upstream_updater_enabled", r"\brun\(\s*game::get_appdata_path\(\)"),
    ("src/client/main.cpp", "upstream_beta_download_enabled", r"boiii-beta\.exe|r2\.ezz\.lol/boiii/beta"),
]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def main() -> int:
    problems = []
    for relative, switch, marker in SWITCHES:
        path = ROOT / relative
        if not path.is_file():
            continue
        code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        declared = re.findall(rf"\bconstexpr\s+bool\s+{switch}\s*=\s*(\w+)\s*;", code)
        if any(value != "false" for value in declared):
            problems.append(f"{relative}: {switch} is not false")
        if re.search(marker, code) and not declared:
            problems.append(f"{relative}: the code {switch} guards is there, but the switch is gone")
        if declared and not re.search(rf"\bif\s*\(\s*!\s*{switch}\b|\bif\s*\(\s*{switch}\s*&&", code):
            problems.append(f"{relative}: {switch} is declared but no longer guards anything")
    for problem in problems:
        print(problem)
    print(f"switches: {'FAIL' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
