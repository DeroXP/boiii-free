"""CREDITS lists everyone who wrote Xerces and what it is built from.

  python -I tools/release/credits.py --check   fails when an author in the Git
                                               history is not in CREDITS
  python -I tools/release/credits.py --write   rewrites the three lists in
                                               CREDITS from the Git history

The lists are the author names Git records (`git log --format=%an`), without
bots (names ending in "[bot]"), in three groups by the date of their first
commit: BOIII (before 20 April 2023, when Ezz BOIII began), Ezz BOIII (up to
the fork, f8f7b735, 4 November 2026) and Xerces (after it). A name stays in
the group of its first commit. The text around the lists is kept as it is.
release.ps1 runs --check, so a release never leaves out a new author.
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CREDITS = ROOT / "CREDITS"
EZZ_START = "2023-04-20"
FORK = "f8f7b7353addb4e3f3c0ac4a9d162bb5615b3b56"
GROUPS = ("BOIII", "Ezz BOIII", "Xerces")


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True,
                          encoding="utf-8").stdout


def authors():
    """Every author by group, in the order of their first commit's group, names sorted."""
    fork_commits = set(git("rev-list", FORK).split())
    first = {}
    for line in git("log", "--reverse", "--format=%H%x09%ad%x09%an", "--date=short").splitlines():
        commit, date, name = line.split("\t", 2)
        name = name.strip()
        if not name or name.endswith("[bot]") or name in first:
            continue
        if commit not in fork_commits:
            first[name] = "Xerces"
        elif date < EZZ_START:
            first[name] = "BOIII"
        else:
            first[name] = "Ezz BOIII"
    groups = {g: sorted((n for n, grp in first.items() if grp == g), key=str.casefold) for g in GROUPS}
    return groups


def listed(text):
    return {line[2:].strip() for line in text.splitlines() if line.startswith("- ")}


def write(text, groups):
    """Replaces each "<!-- <group> -->" ... "<!-- end -->" block with the names."""
    out = text
    for g in GROUPS:
        start, end = f"<!-- {g} -->\n", "<!-- end -->"
        a = out.index(start) + len(start)
        b = out.index(end, a)
        out = out[:a] + "".join(f"- {n}\n" for n in groups[g]) + out[b:]
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--check", action="store_true")
    p.add_argument("--write", action="store_true")
    a = p.parse_args()
    text = CREDITS.read_text(encoding="utf-8").replace("\r\n", "\n")
    groups = authors()
    if a.write:
        CREDITS.write_text(write(text, groups), encoding="utf-8", newline="\n")
        print("CREDITS:", ", ".join(f"{g} {len(groups[g])}" for g in GROUPS))
        return 0
    have = listed(text)
    missing = [n for g in GROUPS for n in groups[g] if n not in have]
    if missing:
        print("CREDITS lacks these authors from the Git history (run credits.py --write):", ", ".join(missing))
        return 1
    print(f"CREDITS lists all {sum(len(v) for v in groups.values())} authors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
