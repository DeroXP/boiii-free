"""Fails when a file the build compiles carries a Microsoft copyright notice.

usage: python tools/ci/check_microsoft_notices.py [--self-test]

src/tlsdll/tlssup.cpp used to be Microsoft C runtime source, which this GPL-3.0
program cannot distribute. It was replaced by a clean-room version on 2026-11-05
(docs/DESIGN.md 16.5). This scan keeps such code out: it reads every C, C++,
assembly, resource, module-definition, IDL, shader and manifest file outside
deps/ (submodules keep their own notices), build/, node_modules and .git, and
fails on a Microsoft copyright line: "Copyright (c) Microsoft Corporation" and
the usual variants ("(C)", the copyright sign, years, "Corp.", no "(c)", the
words wrapped over comment lines).

It has no allowlist. Microsoft code under a free licence (MIT, for example)
comes in through deps/ as a submodule with its own notice.
"""

import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
# Skipped at the top level only.
SKIP_TOP = {"deps", "build"}
# Skipped anywhere.
SKIP_ANY = {".git", "node_modules"}
# What premake projects compile, and what compiled files include or embed.
EXTENSIONS = {
    ".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".h++",
    ".inl", ".inc", ".ipp", ".ixx", ".cppm", ".asm", ".s", ".rc", ".rc2",
    ".def", ".idl", ".manifest", ".hlsl", ".hlsli", ".fx",
}

# Space between words, which may run onto the next comment line. One group, so
# that a "?" after it makes it optional.
GAP = r"(?:(?:[ \t]|\r?\n[ \t]*(?:\*+|//+|;+|#+)?)+)"
SIGN = r"(?:\(c\)|\u00a9|&copy;)"
YEARS = rf"(?:\d{{4}}(?:{GAP}?[-,]{GAP}?\d{{4}})*,?{GAP})?"
NOTICE = re.compile(
    rf"(?:\bCopyright\b(?:{GAP}?{SIGN})?|{SIGN}){GAP}?{YEARS}(?:by{GAP})?"
    rf"Microsoft{GAP}Corp(?:oration\b|\.|\b)",
    re.I,
)


def read_text(path: pathlib.Path) -> str:
    data = path.read_bytes()
    # Resource scripts are often UTF-16.
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16", errors="replace")
    return data.decode("utf-8", errors="replace")


def scanned_files():
    for folder, dirs, files in os.walk(ROOT):
        here = pathlib.Path(folder)
        dirs[:] = sorted(
            d for d in dirs
            if d not in SKIP_ANY and not (here == ROOT and d in SKIP_TOP)
        )
        for name in sorted(files):
            path = here / name
            if path.suffix.lower() in EXTENSIONS:
                yield path


def findings(text: str) -> list[int]:
    """Line numbers where Microsoft copyright notices start."""
    return [text.count("\n", 0, m.start()) + 1 for m in NOTICE.finditer(text)]


def self_test() -> None:
    bad = [
        "Copyright (c) Microsoft Corporation.",
        "*       Copyright (C) Microsoft Corporation",
        "// Copyright \u00a9 Microsoft Corporation.",
        "Copyright (c) 1985-2019 Microsoft Corporation",
        "Copyright 2019, 2020 Microsoft Corp.",
        "(c) Microsoft Corporation",
        "Copyright (c) Microsoft\n * Corporation.",
        "// Copyright (c)\n// Microsoft Corporation",
        "Copyright by Microsoft Corp",
        "VALUE \"LegalCopyright\", \"Copyright (C) Microsoft Corp.\"",
    ]
    good = [
        "Copyright (c) 2026 Xerces contributors",
        "urn:schemas-microsoft-com:asm.v1",
        "Microsoft.Windows.Common-Controls",
        "replaces a file taken from the Microsoft C runtime sources",
        "Microsoft Corporation documents the TLS directory",
        "Copyright (c) Microsoftish Corporation",
        "Copyright (c) momo5502\nMicrosoft Corporation's PE format",
    ]
    for sample in bad:
        assert findings(sample), sample
    for sample in good:
        assert not findings(sample), sample
    assert findings("a\nb\n// Copyright (c) Microsoft Corporation\n") == [3]
    assert findings("x\n/* Copyright (c) Microsoft\n * Corporation */\n") == [2]
    assert ".cpp" in EXTENSIONS and ".rc" in EXTENSIONS and ".md" not in EXTENSIONS
    print("self-test ok")


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        self_test()
        return 0
    problems = []
    count = 0
    for path in scanned_files():
        count += 1
        for line in findings(read_text(path)):
            problems.append(f"{path.relative_to(ROOT).as_posix()}:{line}")
    for problem in problems:
        print(f"Microsoft copyright notice in compiled source: {problem}")
    print(f"checked {count} files: {'FAIL' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
