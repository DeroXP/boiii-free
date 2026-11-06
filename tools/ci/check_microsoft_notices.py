"""Fails when a file the build compiles carries a Microsoft copyright notice.

usage: python tools/ci/check_microsoft_notices.py [--self-test]

src/tlsdll/tlssup.cpp used to be Microsoft C runtime source, which this GPL-3.0
program cannot distribute. It was replaced by a clean-room version on 2026-11-05
(docs/DESIGN.md 16.5). This scan keeps such code out: it reads every C, C++,
assembly, resource, module-definition, IDL, shader and manifest file outside the
top-level deps/ (submodules keep their own notices) and build/, and outside any
node_modules or .git, as UTF-8 or UTF-16 (with or without a byte-order mark).

It fails on a copyright line whose holder is Microsoft. The line starts with
"Copyright", "Copyright (c)", "(c)" or the copyright sign (any letter case),
then optional years ("1985-2019", "1995-96", "(2024)") and "by", then
"Microsoft" (or a joint holder such as "Intel/Microsoft") followed by one of:
- "Corporation" or "Corp" ("Copyright (c) Microsoft Corporation"),
- "Open Technologies",
- ".", ",", ";", ":", a closing quote, the end of a block comment or the end of
  the line ("Copyright (C) Microsoft. All rights reserved.", the short form most
  Windows SDK headers use),
- "All rights reserved".
The words may wrap over comment lines. Text that only mentions Microsoft, such as
"(c) Microsoft's loader" or "Microsoft.Windows.Common-Controls", passes.

It has no allowlist. Microsoft code under a free licence (MIT, for example)
comes in through deps/ as a submodule with its own notice.
"""

import os
import pathlib
import re
import sys
import tempfile

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
# that a "?" after it makes it optional. Every blank belongs to exactly one part
# of it, so a failed match backtracks in linear time; the earlier form,
# (?:[ \t]|\n[ \t]*)+, took exponential time on indented blank lines.
NEWLINE = r"(?:\r?\n[ \t]*(?:(?:\*+|//+|;+|#+)[ \t]*)?)"
GAP = rf"(?:[ \t]+{NEWLINE}*|{NEWLINE}+)"
SIGN = r"(?:\(c\)|©|&copy;)"
# "1985-2019", "2019, 2020", "1995-96", "(2024)".
YEARS = rf"(?:\(?\d{{4}}(?:{GAP}?[-,]{GAP}?\d{{2}}(?:\d{{2}})?)*\)?,?{GAP})?"
# "Microsoft" as the holder, also jointly ("Intel/Microsoft"): what may follow
# it (see the module docstring).
HOLDER = (
    rf"(?:\w+/)?Microsoft(?:{GAP}Corp(?:orations?\b|\.|\b)"
    rf"|{GAP}Open{GAP}Technologies\b"
    rf"|\b(?=[ \t]*(?:[.,;:\"]|\*/|\r?$))"
    rf"|(?={GAP}All{GAP}rights\b))"
)
NOTICE = re.compile(
    rf"(?:\bCopyright\b(?:{GAP}?{SIGN})?|{SIGN}){GAP}?{YEARS}(?:by{GAP})?{HOLDER}",
    re.I | re.M,
)


def decodings(data: bytes) -> list[str]:
    """The texts a file may hold. Resource scripts are often UTF-16."""
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return [data.decode("utf-16", errors="replace")]
    texts = [data.decode("utf-8-sig", errors="replace")]
    # UTF-8 source has no NUL bytes; UTF-16 without a byte-order mark has one
    # in most characters. The wrong byte order decodes to text that matches
    # nothing, so both are scanned.
    if b"\0" in data:
        texts += [data.decode(c, errors="replace") for c in ("utf-16-le", "utf-16-be")]
    return texts


def scanned_files(root: pathlib.Path = ROOT):
    for folder, dirs, files in os.walk(root):
        here = pathlib.Path(folder)
        dirs[:] = sorted(
            d for d in dirs
            if d not in SKIP_ANY and not (here == root and d in SKIP_TOP)
        )
        for name in sorted(files):
            path = here / name
            if path.suffix.lower() in EXTENSIONS:
                yield path


def findings(text: str) -> list[int]:
    """Line numbers where Microsoft copyright notices start."""
    return [text.count("\n", 0, m.start()) + 1 for m in NOTICE.finditer(text)]


def scan(root: pathlib.Path = ROOT) -> tuple[int, list[str]]:
    """The number of files read, and "path:line" for each notice found."""
    count = 0
    problems = []
    for path in scanned_files(root):
        count += 1
        lines = sorted({n for text in decodings(path.read_bytes()) for n in findings(text)})
        problems += [f"{path.relative_to(root).as_posix()}:{n}" for n in lines]
    return count, problems


def check(condition: bool, what: object) -> None:
    # Not assert, which python -O drops.
    if not condition:
        raise SystemExit(f"self-test failed: {what!r}")


def self_test_patterns() -> None:
    bad = [
        "Copyright (c) Microsoft Corporation.",
        "*       Copyright (C) Microsoft Corporation",
        "// Copyright © Microsoft Corporation.",
        "Copyright (c) 1985-2019 Microsoft Corporation",
        "Copyright 2019, 2020 Microsoft Corp.",
        "(c) Microsoft Corporation",
        "Copyright (c) Microsoft\n * Corporation.",
        "// Copyright (c)\n// Microsoft Corporation",
        "Copyright by Microsoft Corp",
        "VALUE \"LegalCopyright\", \"Copyright (C) Microsoft Corp.\"",
        "COPYRIGHT (C) MICROSOFT CORPORATION",
        "Copyright (c) Microsoft Corporation. Licensed under the MIT License.",
        # The short forms in Windows SDK 10.0.26100.0 headers, for example
        # winrt/rometadata.h line 2.
        "// Copyright (c) Microsoft. All rights reserved.",
        "*  Copyright (C) Microsoft.  All rights reserved.",
        "Copyright (C) Microsoft. All rights reserved",
        "// Copyright (c) Microsoft\r\n// All rights reserved.",
        "Copyright (c) Microsoft",
        "Copyright (c) Microsoft\n",
        "/* Copyright (c) Microsoft */",
        "VALUE \"LegalCopyright\", \"Copyright (C) Microsoft\"",
        "Copyright (c) 2015, Microsoft. Licensed under the MIT License.",
        "Copyright (c) Microsoft All rights reserved.",
        "Copyright (c) Microsoft Open Technologies, Inc.",
        # Other forms in that SDK.
        "Copyright (c) 1990,91 Microsoft Corporation",
        "Copyright (c) 1985-96, Microsoft Corporation",
        "Copyright (2024) Microsoft. All rights reserved.",
        "Copyright (C) 1994-1999 Intel/Microsoft Corporation.  All Rights Reserved.",
        "Portions copyright 1992, 1993 Intel/Microsoft, all rights reserved.",
        "* Copyright (c) Microsoft Corporations. All Rights Reserved.",
    ]
    good = [
        "Copyright (c) 2026 Xerces contributors",
        "urn:schemas-microsoft-com:asm.v1",
        "Microsoft.Windows.Common-Controls",
        "replaces a file taken from the Microsoft C runtime sources",
        "Microsoft Corporation documents the TLS directory",
        "Copyright (c) Microsoftish Corporation",
        "Copyright (c) Microsoftish.",
        "Copyright (c) momo5502\nMicrosoft Corporation's PE format",
        "(a) the PE format, (b) the loader, (c) Microsoft's documentation",
        "(c) Microsoft documents the TLS directory",
        "VALUE \"LegalCopyright\", \"Copyright (C) 2026\"\n// Microsoft.",
    ]
    # Indented blank lines: these took the earlier GAP exponential time.
    blank_lines = ("\n" + " " * 40) * 30 + "x"
    bad += [
        "Copyright (c) Microsoft" + blank_lines,
        "Copyright (c) Microsoft" + (" " * 40 + "\n") * 30 + "x",
    ]
    good += [
        "Copyright (c)" + blank_lines,
        "Copyright 2019" + blank_lines,
        "Copyright by" + blank_lines,
        "(c)" + blank_lines.replace("\n", "\r\n"),
    ]
    for sample in bad:
        check(findings(sample), sample)
    for sample in good:
        check(not findings(sample), sample)
    check(findings("a\nb\n// Copyright (c) Microsoft Corporation\n") == [3], "line")
    check(findings("x\n/* Copyright (c) Microsoft\n * Corporation */\n") == [2], "wrap")
    check(findings("x\r\n// Copyright (C) Microsoft.\r\n") == [2], "crlf")
    check(".cpp" in EXTENSIONS and ".rc" in EXTENSIONS and ".md" not in EXTENSIONS, "ext")


def self_test_files() -> None:
    notice = "// Copyright (C) Microsoft. All rights reserved.\n"
    clean = "// Copyright (c) 2026 Xerces contributors\n"
    files = {
        # Scanned, with a notice.
        "src/a.cpp": notice.encode(),
        "src/UPPER.CPP": notice.encode(),
        "src/bom.cpp": b"\xef\xbb\xbf" + notice.encode(),
        "src/deps/a.cpp": notice.encode(),
        "src/build/a.h": notice.encode(),
        "src/tlsdll/utf16-bom.rc": ("﻿#include <winres.h>\r\n" + notice).encode("utf-16-le"),
        "src/tlsdll/utf16le.rc": ("#include <winres.h>\r\n" + notice).encode("utf-16-le"),
        "src/tlsdll/utf16be.rc": ("#include <winres.h>\r\n" + notice).encode("utf-16-be"),
        # Scanned, clean.
        "src/clean.cpp": clean.encode(),
        "src/clean16.rc": clean.encode("utf-16-le"),
        # Not scanned.
        "deps/sub/a.cpp": notice.encode(),
        "build/src/a.cpp": notice.encode(),
        "src/node_modules/pkg/a.h": notice.encode(),
        "node_modules/a.c": notice.encode(),
        ".git/a.cpp": notice.encode(),
        "src/readme.md": notice.encode(),
        "src/a.cpp.txt": notice.encode(),
    }
    with tempfile.TemporaryDirectory(prefix="xerces-notices-") as tmp:
        root = pathlib.Path(tmp)
        for name, data in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(data)
        count, problems = scan(root)
    expected = [
        "src/a.cpp:1", "src/UPPER.CPP:1", "src/bom.cpp:1", "src/deps/a.cpp:1",
        "src/build/a.h:1", "src/tlsdll/utf16-bom.rc:2", "src/tlsdll/utf16le.rc:2",
        "src/tlsdll/utf16be.rc:2",
    ]
    check(sorted(problems) == sorted(expected), problems)
    check(count == 10, count)


def self_test() -> None:
    self_test_patterns()
    self_test_files()
    print("self-test ok")


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        self_test()
        return 0
    count, problems = scan()
    for problem in problems:
        print(f"Microsoft copyright notice in compiled source: {problem}")
    print(f"checked {count} files: {'FAIL' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
