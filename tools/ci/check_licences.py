"""Fails when code outside deps/ carries a licence that GPL-3.0 cannot take in.

usage: python tools/ci/check_licences.py [--self-test]

Xerces is GPL-3.0 (docs/DESIGN.md 16.5). GPL-2.0-only code cannot be combined
with it, and src/common/str.hpp once held such code. The scan reads the text
files under the repository root (EXTENSIONS and NAMES). It skips deps/
(submodules with their own notices), docs/ (prose about licences), build output
and node_modules. Each finding has a kind:

- "spdx": an SPDX licence expression that is not GPL-3.0-compatible, or that
  this script does not know (review it, then add it to COMPATIBLE);
- "spdx-review": AGPL and OFL identifiers. GPL-3.0 can take them in, but they
  bring duties of their own (16.5: servers offer their source; fonts ship
  unmodified), so each file needs a KNOWN entry;
- "gpl2": a GPL version 2 notice or tag with no "or later" after it. This covers
  the FSF wordings ("version 2 as published by the Free Software Foundation",
  "Foundation; version 2 of the License", "version 2, as published") and short
  tags (GPLv2, GPL v2, GPL-2.0, GPL-2.0-only, GPL version 2, GNU General Public
  License v2). Tags inside SPDX lines are left to the SPDX check;
- "kernel": Linux kernel source headers (linux/lib/string.c and the like) and
  Linus Torvalds' copyright lines;
- "microsoft": Microsoft source with all rights reserved, unless an MIT, Apache
  or BSD licence line is within a few lines of it.

The patterns cover the usual wordings, not every possible one, so a person
still reads what an upstream merge brings in. Prose that only names the Linux
kernel is not flagged (str.hpp names the kernel API it follows).

KNOWN lists reviewed findings by path and kind, with the plan or the decision.
Only that kind is excused in that file. Any other finding there still fails,
and so does an entry that no longer matches anything.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SKIP_DIRS = {"deps", "docs", "build", "node_modules", ".git"}
EXTENSIONS = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".ipp",
    ".asm", ".s", ".rc", ".lua", ".py", ".ps1", ".psm1", ".sh", ".bat", ".cmd",
    ".js", ".html", ".css", ".rml", ".rcss", ".gsc", ".csc", ".gsh", ".toml",
    ".yml", ".yaml", ".json", ".md", ".txt",
}
# Files without one of the extensions above that are still read.
NAMES = {"LICENSE", "LICENCE", "COPYING", "NOTICE", "Dockerfile"}

# SPDX identifiers GPL-3.0 can take in (FSF compatibility list; the -or-later
# forms of GPL-2.0 and LGPL-2.x reach version 3).
COMPATIBLE = {
    "0BSD", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "BSL-1.0", "CC0-1.0",
    "FTL", "ISC", "MIT", "MIT-0", "MPL-2.0", "Unlicense", "X11", "Zlib",
    "zlib-acknowledgement", "GPL-2.0-or-later", "GPL-2.0+", "GPL-3.0",
    "GPL-3.0-only", "GPL-3.0-or-later", "GPL-3.0+", "LGPL-2.1", "LGPL-2.1-only",
    "LGPL-2.1-or-later", "LGPL-2.1+", "LGPL-2.0-or-later", "LGPL-2.0+",
    "LGPL-3.0", "LGPL-3.0-only", "LGPL-3.0-or-later", "LGPL-3.0+",
}
# Allowed, but each file needs a KNOWN entry: AGPL code makes servers offer
# their source, and the FSF lists OFL for fonts, not for code.
NEEDS_REVIEW = {"AGPL-3.0", "AGPL-3.0-only", "AGPL-3.0-or-later", "OFL-1.1"}
# Exceptions an SPDX "WITH" may name.
COMPATIBLE_EXCEPTIONS = {"LLVM-exception", "GCC-exception-3.1", "Classpath-exception-2.0"}

SPDX = re.compile(r"SPDX-License-Identifier:\s*([^\n*]+?)\s*(?:\*/|-->|$)", re.M)
SPDX_LINE = re.compile(r"^.*SPDX-License-Identifier:.*$", re.M)

# Comment markers at the start of a line. They are dropped so that a notice
# wrapped over several comment lines reads as one sentence.
COMMENT_LEADER = re.compile(r"^\s*(?:/\*+|\*+|//+|#+|;+|--+|<!--)?\s*")

# The FSF's GPL version 2 notice wordings.
GPL2_NOTICE = re.compile(
    r"version\s+2\s*,?\s+as\s+published\s+by\s+the\s+Free\s+Software\s+Foundation"
    r"|Free\s+Software\s+Foundation\s*[;,]?\s+version\s+2\s+of\s+the\s+License",
    re.I,
)
# Short tags. "Lesser" and "Library" GPL 2.x can move to GPL-3.0, so they are
# not matched; neither is 2.1 or any other 2.x, nor a link such as
# gnu.org/licenses/gpl-2.0.html (or-later notices link there too).
GPL2_TAG = re.compile(
    r"(?:(?<!/)\bGPL|(?<!Lesser )(?<!Library )\bGeneral\s+Public\s+License)"
    r"\s*[-,(]?\s*(?:v|version\s*)?\s*2(?:\.0)?(?:-only)?(?![.,]?\d)",
    re.I,
)
# Words after a version 2 match that make it "or later". Only the rest of the
# sentence is searched.
LATER = re.compile(r"\A\s*\+|or[\s-]+later|any\s+later\s+version", re.I)
SENTENCE_END = re.compile(r"\.(?:\s|\Z)")

KERNEL_PATH = re.compile(r"\blinux/(?:lib|kernel|mm|fs|net|drivers|arch|include|init|ipc|block|crypto|security|sound)/[\w/-]+\.[ch]\b")
# "(C)" or a copyright sign (any one symbol), then years.
KERNEL_COPYRIGHT = re.compile(r"Copyright\s*(?:\(c\)|\W)?[\s\d,-]*Linus\s+Torvalds", re.I)

MICROSOFT = re.compile(r"Copyright\s+\(c\)\s+Microsoft\s+Corporation\.?\s+All\s+rights\s+reserved", re.I)
# Microsoft publishes some code under these (Detours is MIT, the WebView2 SDK
# BSD).
PERMISSIVE = re.compile(
    r"Licen[cs]ed\s+under\s+the\s+(?:MIT|Apache)|\bMIT\s+Licen[cs]e|Apache\s+Licen[cs]e"
    r"|\bBSD[\s-]+(?:style\s+)?licen[cs]e|Redistribution\s+and\s+use\s+in\s+source\s+and\s+binary\s+forms"
    r"|SPDX-License-Identifier:\s*(?:MIT|Apache-2\.0|BSD-[23]-Clause)",
    re.I,
)
# Lines before and after a Microsoft copyright searched for PERMISSIVE.
PERMISSIVE_LINES_BEFORE = 2
PERMISSIVE_LINES_AFTER = 5

# (path, kind) -> what is planned or was decided.
KNOWN = {
    ("src/tlsdll/tlssup.cpp", "microsoft"): (
        "Microsoft CRT source; a clean-room rewrite is a 16.5 gate, after the "
        "fence review of the loader and TLS helper"
    ),
}

RANK = {"ok": 0, "review": 1, "bad": 2}


def spdx_verdict(expression: str) -> str:
    """Returns "ok", "review" or "bad". An OR takes its best side, an AND its
    worst term."""
    expression = expression.replace("(", " ").replace(")", " ")
    best = "bad"
    for alternative in re.split(r"\s+OR\s+", expression):
        verdict = "ok"
        for part in re.split(r"\s+AND\s+", alternative.strip()):
            licence, _, exception = part.partition(" WITH ")
            licence = licence.strip()
            if licence in COMPATIBLE:
                term = "ok"
            elif licence in NEEDS_REVIEW:
                term = "review"
            else:
                term = "bad"
            if exception and exception.strip() not in COMPATIBLE_EXCEPTIONS:
                term = "bad"
            if RANK[term] > RANK[verdict]:
                verdict = term
        if RANK[verdict] < RANK[best]:
            best = verdict
    return best


def flatten(text: str) -> str:
    """The text on one line, comment markers dropped, spaces collapsed."""
    lines = (COMMENT_LEADER.sub("", line, count=1) for line in text.splitlines())
    return re.sub(r"\s+", " ", " ".join(lines))


def or_later_follows(flat: str, end: int) -> bool:
    rest = flat[end : end + 200]
    stop = SENTENCE_END.search(rest)
    if stop:
        rest = rest[: stop.start()]
    return bool(LATER.search(rest))


def permissive_nearby(text: str, position: int) -> bool:
    lines = text.split("\n")
    line = text.count("\n", 0, position)
    first = max(0, line - PERMISSIVE_LINES_BEFORE)
    window = "\n".join(lines[first : line + PERMISSIVE_LINES_AFTER + 1])
    return bool(PERMISSIVE.search(window))


def problems_in(text: str) -> list[tuple[str, str]]:
    """(kind, message) for every finding, each message once."""
    found = []
    for match in SPDX.finditer(text):
        expression = match.group(1).strip()
        verdict = spdx_verdict(expression)
        if verdict == "bad":
            found.append(("spdx", f"SPDX identifier '{expression}' is not GPL-3.0-compatible (or not reviewed)"))
        elif verdict == "review":
            found.append(("spdx-review", f"SPDX identifier '{expression}' needs a review (AGPL or OFL)"))

    flat = flatten(SPDX_LINE.sub("", text))
    for pattern in (GPL2_NOTICE, GPL2_TAG):
        for match in pattern.finditer(flat):
            if not or_later_follows(flat, match.end()):
                found.append(("gpl2", f"GPL version 2 only ('{match.group()}')"))

    for match in KERNEL_PATH.finditer(text):
        found.append(("kernel", f"Linux kernel source header ({match.group()})"))
    if KERNEL_COPYRIGHT.search(flat):
        found.append(("kernel", "Linux kernel copyright (Linus Torvalds)"))

    for match in MICROSOFT.finditer(text):
        if not permissive_nearby(text, match.start()):
            found.append(("microsoft", "Microsoft source (all rights reserved)"))
    return list(dict.fromkeys(found))


def sort_findings(findings: dict[str, list[tuple[str, str]]]) -> tuple[list[str], list[str]]:
    """Splits findings into known items and problems. KNOWN entries that
    matched nothing are problems too."""
    known, problems, used = [], [], set()
    for relative, items in findings.items():
        for kind, message in items:
            key = (relative, kind)
            if key in KNOWN:
                used.add(key)
                known.append(f"{relative}: {message} (known: {KNOWN[key]})")
            else:
                problems.append(f"{relative}: {message}")
    for path, kind in KNOWN:
        if (path, kind) not in used:
            problems.append(f"{path}: KNOWN entry for '{kind}' matches nothing; remove it")
    return known, problems


def candidate_files():
    for path in sorted(ROOT.rglob("*")):
        relative = path.relative_to(ROOT)
        if relative.parts and relative.parts[0] in SKIP_DIRS:
            continue
        if "node_modules" in relative.parts or not path.is_file():
            continue
        if path.suffix.lower() not in EXTENSIONS and path.name not in NAMES:
            continue
        if path.resolve() == pathlib.Path(__file__).resolve():
            continue
        yield path, relative.as_posix()


def self_test() -> int:
    # (text, the kinds it must give)
    cases = [
        ("// SPDX-License-Identifier: GPL-2.0\n", {"spdx"}),
        ("/* SPDX-License-Identifier: GPL-2.0-only */\n", {"spdx"}),
        ("# SPDX-License-Identifier: Proprietary\n", {"spdx"}),
        ("// SPDX-License-Identifier: AGPL-3.0-only\n", {"spdx-review"}),
        ("// SPDX-License-Identifier: OFL-1.1\n", {"spdx-review"}),
        ("// SPDX-License-Identifier: AGPL-3.0-or-later OR GPL-2.0-only\n", {"spdx-review"}),
        ("// SPDX-License-Identifier: MIT AND Proprietary\n", {"spdx"}),
        (" * under the terms of the GNU General Public License version 2 as published by the Free Software Foundation.\n", {"gpl2"}),
        (" * the GNU General Public License as published by the Free Software Foundation; version 2 of the License.\n", {"gpl2"}),
        (" * GNU General Public License,\n * version 2, as published by the Free Software Foundation.\n", {"gpl2"}),
        (" * it under the terms of the GNU General Public License version 2 as\n * published by the Free Software Foundation.\n", {"gpl2"}),
        ("/* Released under the GPLv2 only. */\n", {"gpl2"}),
        ("// Licensed under the GPL v2.\n", {"gpl2"}),
        ("# License: GPL-2.0-only\n", {"gpl2"}),
        ('MODULE_LICENSE("GPL v2");\n', {"gpl2"}),
        ("// Licensed under the GPL v2. Needs Windows 10 or later.\n", {"gpl2"}),
        ('  "license": "GPL-2.0",\n', {"gpl2"}),
        ("GNU GENERAL PUBLIC LICENSE\n Version 2, June 1991\n", {"gpl2"}),
        (" *  linux/lib/string.c\n", {"kernel"}),
        (" *  Copyright (C) 1991, 1992  Linus Torvalds\n", {"kernel"}),
        (" *       Copyright (c) Microsoft Corporation. All rights reserved.\n", {"microsoft"}),
        ("// Copyright (c) Microsoft Corporation. All rights reserved.\n//\n//\n//\n//\n//\n//\n// Licensed under the MIT License.\n", {"microsoft"}),
        ("// SPDX-License-Identifier: MIT\n", set()),
        ("// SPDX-License-Identifier: GPL-2.0-or-later\n", set()),
        ("// SPDX-License-Identifier: GPL-2.0 OR MIT\n", set()),
        ("// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception\n", set()),
        ("/* SPDX-License-Identifier: BSD-3-Clause */\n", set()),
        (" * either version 2 of the License, or (at your option) any later version.\n", set()),
        (" * the Free Software Foundation; version 2 as published by the Free Software Foundation, or any later version.\n", set()),
        (" * it under the terms of the GNU General Public License as published by\n * the Free Software Foundation; either version 2 of the License, or\n * (at your option) any later version.\n", set()),
        ("// Licensed under GPLv2+.\n", set()),
        ("# License: GPL-2.0-or-later\n", set()),
        ("// GPL v2 or later.\n", set()),
        (" * GNU Lesser General Public License version 2.1 as published by the Free Software Foundation.\n", set()),
        (" * GNU Library General Public License version 2 as published by the Free Software Foundation, or any later version.\n", set()),
        ("Free software under the GNU General Public License, version 3.\n", set()),
        ("// LGPLv2.1 and AGPLv3 code\n", set()),
        ("// The names follow the Linux kernel's documented strscpy API.\n", set()),
        ("//  Copyright (c) Microsoft Corporation.  All rights reserved.\n//  Licensed under the MIT License.\n", set()),
        (" * Copyright (c) Microsoft Corporation. All rights reserved.\n * SPDX-License-Identifier: Apache-2.0\n", set()),
        ("// Copyright (C) Microsoft Corporation. All rights reserved.\n// Use of this source code is governed by a BSD-style license that can be\n", set()),
        ("# GPL version 2 or later <https://www.gnu.org/licenses/gpl-2.0.html>. You must apply it.\n", set()),
    ]
    failures = []
    for text, expected in cases:
        kinds = {kind for kind, _ in problems_in(text)}
        if kinds != expected:
            failures.append(f"{text.strip()!r}: gave {sorted(kinds)}, wants {sorted(expected)}")

    # KNOWN excuses one kind in one file; anything else there fails, and an
    # entry that matches nothing fails.
    tls = "src/tlsdll/tlssup.cpp"
    known, problems = sort_findings({tls: [("microsoft", "m"), ("gpl2", "g")]})
    if len(known) != 1 or problems != [f"{tls}: g"]:
        failures.append(f"KNOWN by path and kind: known {known}, problems {problems}")
    known, problems = sort_findings({})
    if known or len(problems) != len(KNOWN):
        failures.append(f"unused KNOWN entries: known {known}, problems {problems}")

    for line in failures:
        print(f"self-test: {line}")
    print(f"licences self-test: {'FAIL' if failures else 'ok'}")
    return 1 if failures else 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()
    findings = {}
    for path, relative in candidate_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        items = problems_in(text)
        if items:
            findings[relative] = items
    known, problems = sort_findings(findings)
    for line in known:
        print(f"known licence item: {line}")
    for line in problems:
        print(f"licence problem: {line}")
    print(f"licences: {'FAIL' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
