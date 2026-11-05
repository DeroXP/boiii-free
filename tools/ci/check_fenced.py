"""Fails when a Xerces commit changes fenced code (docs/FENCED.md, enforcement).

usage: python tools/ci/check_fenced.py [--base REF] [--head REF] [--self-test]

- Whole-file fences (tools/ci/fences.json "files", glob patterns): any change
  in a non-merge commit fails.
- Partial fences: each named function ("functions") and each set of lines
  containing a marker ("lines") is hashed over its normalized tokens in the
  commit and its parent; any difference fails.
- A commit that touches a file holding a partial fence must say "fence
  reviewed" in its message, or be listed in tools/ci/fence_reviews.txt.
- Merge commits (upstream syncs) are exempt.

The default range starts at the pinned upstream base in fences.json ("base")
and ends at HEAD.
"""

import argparse
import fnmatch
import hashlib
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def git(*args: str, check: bool = True) -> str:
    result = subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
        errors="replace",
    )
    if check and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def show(commit: str, path: str) -> str | None:
    result = subprocess.run(
        ["git", "show", f"{commit}:{path}"], cwd=ROOT, capture_output=True,
        text=True, encoding="utf-8", errors="replace",
    )
    return result.stdout if result.returncode == 0 else None


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def normalized_hash(text: str) -> str:
    tokens = re.findall(r"[A-Za-z_]\w*|\d[\w.]*|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|\S", strip_comments(text))
    return hashlib.sha256("\x1f".join(tokens).encode("utf-8")).hexdigest()


def function_body(text: str, name: str) -> str | None:
    """The definition of a C++ function: its signature line through the
    matching closing brace. None when it is not defined in the text."""
    code = strip_comments(text)
    for match in re.finditer(rf"(?m)^[ \t]*[^\n;{{}}()=]*\b{re.escape(name)}\s*\(", code):
        start = match.start()
        brace = code.find("{", match.end())
        semicolon = code.find(";", match.end())
        if brace < 0 or (0 <= semicolon < brace and code[match.end():semicolon].count("(") <= code[match.end():semicolon].count(")")):
            continue  # a declaration or a call
        depth = 0
        for i in range(brace, len(code)):
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    return code[start : i + 1]
        return None
    return None


def marked_lines(text: str, markers: list[str]) -> str:
    return "\n".join(line for line in text.splitlines() if any(m in line for m in markers))


def load_fences() -> dict:
    return json.loads((ROOT / "tools" / "ci" / "fences.json").read_text(encoding="utf-8"))


def reviewed_commits() -> set[str]:
    path = ROOT / "tools" / "ci" / "fence_reviews.txt"
    if not path.exists():
        return set()
    out = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.add(line.split()[0])
    return out


def matches_file_fence(path: str, patterns: list[str]) -> bool:
    for pattern in patterns:
        if path == pattern or fnmatch.fnmatchcase(path, pattern):
            return True
        if pattern.endswith("/**") and path.startswith(pattern[:-2]):
            return True
    return False


def check_commit(commit: str, fences: dict, reviewed: set[str]) -> list[str]:
    parents = git("rev-list", "--parents", "-n", "1", commit).split()[1:]
    if len(parents) != 1:
        return []  # merges (upstream syncs) are exempt; the root has no parent
    parent = parents[0]
    changed = [p for p in git("diff", "--name-only", parent, commit).splitlines() if p]
    message = git("log", "-1", "--format=%B", commit)
    problems = []
    for path in changed:
        if matches_file_fence(path, fences["files"]):
            problems.append(f"{commit[:8]} changes fenced file {path}")
    partial = set(fences.get("functions", {})) | set(fences.get("lines", {}))
    for path in changed:
        if path not in partial:
            continue
        before = show(parent, path) or ""
        after = show(commit, path) or ""
        for name in fences.get("functions", {}).get(path, []):
            old = function_body(before, name)
            new = function_body(after, name)
            if old is None or new is None:
                problems.append(f"{commit[:8]} {path}: fenced function {name} not found")
            elif normalized_hash(old) != normalized_hash(new):
                problems.append(f"{commit[:8]} {path}: changes fenced function {name}")
        markers = fences.get("lines", {}).get(path, [])
        if markers and normalized_hash(marked_lines(before, markers)) != normalized_hash(marked_lines(after, markers)):
            problems.append(f"{commit[:8]} {path}: changes fenced lines ({', '.join(markers)})")
        if "fence reviewed" not in message.lower() and commit not in reviewed and commit[:8] not in reviewed:
            problems.append(f"{commit[:8]} touches {path}, which holds fenced code; "
                            "say 'fence reviewed' in the message or list it in tools/ci/fence_reviews.txt")
    return problems


def self_test() -> None:
    code = "void f();\nvoid f() {\n  if (x) { y(); } // note\n}\nint g() { return 1; }\n"
    body = function_body(code, "f")
    assert body is not None and body.strip().startswith("void f()") and body.strip().endswith("}"), body
    assert function_body(code, "missing") is None
    assert normalized_hash("a  b // c") == normalized_hash("a b")
    assert normalized_hash("a b") != normalized_hash("a c")
    assert matches_file_fence("src/client/steam/x/y.cpp", ["src/client/steam/**"])
    assert matches_file_fence("src/common/utils/hardware_breakpoint.hpp", ["src/common/utils/hardware_breakpoint.*"])
    assert not matches_file_fence("src/client/component/auth_extra.cpp", ["src/client/component/auth.cpp"])
    print("self-test ok")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    fences = load_fences()
    base = args.base or fences.get("base") or git("merge-base", "upstream/beta", args.head).strip()
    reviewed = reviewed_commits()
    problems = []
    commits = git("rev-list", "--reverse", f"{base}..{args.head}").split()
    for commit in commits:
        problems.extend(check_commit(commit, fences, reviewed))
    # Every anchor must still exist at the head, or the fence silently lapses.
    for path, names in fences.get("functions", {}).items():
        text = show(args.head, path)
        if text is None:
            problems.append(f"fenced file {path} no longer exists at {args.head}")
            continue
        for name in names:
            if function_body(text, name) is None:
                problems.append(f"{path}: fenced function {name} not found at {args.head}")
    for problem in problems:
        print(problem)
    print(f"checked {len(commits)} commits: {'FAIL' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
