"""Fails when code outside deps/ carries a licence that GPL-3.0 cannot take in.

usage: python tools/ci/check_licences.py [--self-test]

Xerces is GPL-3.0 (docs/DESIGN.md 16.5). GPL-2.0-only code cannot be combined
with it, so an upstream merge must not bring such code back the way
src/common/str.hpp once did. The scan reads source and script files under the
repository root, skipping deps/ (submodules with their own notices), docs/ (prose
about licences), build output and node_modules. It flags:

- SPDX licence identifiers whose expression is not GPL-3.0-compatible, or that
  this script does not know (review them, then add them to COMPATIBLE);
- GPL version 2 only notices ("version 2 as published by the Free Software
  Foundation", with no "or any later version");
- Linux kernel source headers ("linux/lib/string.c" and the like);
- Microsoft runtime source ("Microsoft Corporation. All rights reserved.").

Files that are known and have a plan are listed in KNOWN with that plan; they
are reported but do not fail the scan.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SKIP_DIRS = {"deps", "docs", "build", "node_modules", ".git"}
EXTENSIONS = {
    ".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".rc", ".lua", ".py", ".ps1",
    ".psm1", ".sh", ".bat", ".cmd", ".js", ".html", ".rml", ".rcss", ".gsc",
    ".csc", ".toml", ".yml", ".yaml",
}

# SPDX identifiers GPL-3.0 can take in (FSF compatibility list; the -or-later
# forms of GPL-2.0 and LGPL-2.x reach version 3).
COMPATIBLE = {
    "0BSD", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "BSL-1.0", "CC0-1.0",
    "FTL", "ISC", "MIT", "MIT-0", "MPL-2.0", "OFL-1.1", "Unlicense", "X11",
    "Zlib", "zlib-acknowledgement", "GPL-2.0-or-later", "GPL-2.0+",
    "GPL-3.0", "GPL-3.0-only", "GPL-3.0-or-later", "GPL-3.0+",
    "LGPL-2.1", "LGPL-2.1-only", "LGPL-2.1-or-later", "LGPL-2.1+",
    "LGPL-2.0-or-later", "LGPL-2.0+", "LGPL-3.0", "LGPL-3.0-only",
    "LGPL-3.0-or-later", "LGPL-3.0+", "AGPL-3.0", "AGPL-3.0-only",
    "AGPL-3.0-or-later",
}
# Exceptions an SPDX "WITH" may name.
COMPATIBLE_EXCEPTIONS = {"LLVM-exception", "GCC-exception-3.1", "Classpath-exception-2.0"}

SPDX = re.compile(r"SPDX-License-Identifier:\s*([^\n*]+?)\s*(?:\*/|-->|$)", re.M)
GPL2_ONLY = re.compile(r"version\s+2\s+as\s+published\s+by\s+the\s+Free\s+Software\s+Foundation", re.I)
LATER = re.compile(r"any\s+later\s+version", re.I)
KERNEL_PATH = re.compile(r"\blinux/(?:lib|kernel|mm|fs|net|drivers|arch|include|init|ipc|block|crypto|security|sound)/[\w/-]+\.[ch]\b")
MICROSOFT = re.compile(r"Copyright\s+\(c\)\s+Microsoft\s+Corporation\.?\s+All\s+rights\s+reserved", re.I)

# path -> what is planned for it.
KNOWN = {
    "src/tlsdll/tlssup.cpp": (
        "Microsoft CRT source; a clean-room rewrite is a 16.5 gate, after the "
        "fence review of the loader and TLS helper"
    ),
}


def spdx_ok(expression: str) -> bool:
    """True when every term is compatible; for OR, when one side is."""
    expression = expression.replace("(", " ").replace(")", " ")
    for alternative in re.split(r"\s+OR\s+", expression):
        parts = re.split(r"\s+AND\s+", alternative.strip())
        good = True
        for part in parts:
            licence, _, exception = part.partition(" WITH ")
            if licence.strip() not in COMPATIBLE:
                good = False
            if exception and exception.strip() not in COMPATIBLE_EXCEPTIONS:
                good = False
        if good:
            return True
    return False


def problems_in(text: str) -> list[str]:
    found = []
    for match in SPDX.finditer(text):
        expression = match.group(1).strip()
        if not spdx_ok(expression):
            found.append(f"SPDX identifier '{expression}' is not GPL-3.0-compatible (or not reviewed)")
    for match in GPL2_ONLY.finditer(text):
        # The or-later notice puts "or (at your option) any later version"
        # in the same sentence.
        window = text[match.start() : match.end() + 200]
        if not LATER.search(window):
            found.append("GPL version 2 only notice")
    for match in KERNEL_PATH.finditer(text):
        found.append(f"Linux kernel source header ({match.group()})")
    if MICROSOFT.search(text):
        found.append("Microsoft source (all rights reserved)")
    return found


def candidate_files():
    for path in sorted(ROOT.rglob("*")):
        relative = path.relative_to(ROOT)
        if relative.parts and relative.parts[0] in SKIP_DIRS:
            continue
        if "node_modules" in relative.parts or not path.is_file():
            continue
        if path.suffix.lower() not in EXTENSIONS and path.name != "premake5.lua":
            continue
        if path.resolve() == pathlib.Path(__file__).resolve():
            continue
        yield path, relative.as_posix()


def self_test() -> int:
    bad = [
        "// SPDX-License-Identifier: GPL-2.0\n",
        "/* SPDX-License-Identifier: GPL-2.0-only */\n",
        "# SPDX-License-Identifier: Proprietary\n",
        " * under the terms of the GNU General Public License version 2 as published by the Free Software Foundation.\n",
        " *  linux/lib/string.c\n",
        " *       Copyright (c) Microsoft Corporation. All rights reserved.\n",
    ]
    good = [
        "// SPDX-License-Identifier: MIT\n",
        "// SPDX-License-Identifier: GPL-2.0-or-later\n",
        "// SPDX-License-Identifier: GPL-2.0 OR MIT\n",
        "// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception\n",
        "/* SPDX-License-Identifier: BSD-3-Clause */\n",
        " * either version 2 of the License, or (at your option) any later version.\n",
        " * the Free Software Foundation; version 2 as published by the Free Software Foundation, or any later version.\n",
        "// The file it replaced held Linux kernel code (GPL-2.0-only).\n",
    ]
    failures = [line for line in bad if not problems_in(line)]
    failures += [line for line in good if problems_in(line)]
    for line in failures:
        print(f"self-test: wrong verdict for {line.strip()!r}")
    print(f"licences self-test: {'FAIL' if failures else 'ok'}")
    return 1 if failures else 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()
    problems = []
    known = []
    for path, relative in candidate_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for problem in problems_in(text):
            if relative in KNOWN:
                known.append(f"{relative}: {problem} (known: {KNOWN[relative]})")
            else:
                problems.append(f"{relative}: {problem}")
    for line in known:
        print(f"known licence item: {line}")
    for line in problems:
        print(f"licence problem: {line}")
    print(f"licences: {'FAIL' if problems else 'ok'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
