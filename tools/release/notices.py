"""Writes THIRD_PARTY_NOTICES.txt and SOURCE.txt for a Xerces package (docs/DESIGN.md 16.1, 16.5).

usage: python -I tools/release/notices.py [--build-dir build] [--third-party DIR]
           [--notices FILE] [--source FILE] [--downloads DIR] [--check]
           [--licences] [--self-test]

THIRD_PARTY_NOTICES.txt has every library compiled into the shipped programs
(boiii.exe, Xerces.exe, xerces_cli.exe) with its licence text, read from the
licence files themselves. Which libraries those are comes from the build:
the link lines premake wrote into <build-dir>/*.ninja, and the sources that
include the header-only ones. A linked library this script does not know
fails the run, so the notices cannot quietly fall behind the build.

SOURCE.txt names the commit on github.com/DeroXP/boiii-free, the submodule
commits, and the third-party source archives built into Xerces.exe (RmlUi,
FreeType and, once the launcher builds it, HarfBuzz) with their SHA-256. The
archives are read from --downloads; a hash docs/TESTING.md lists must match.

THIRD_PARTY_NOTICES.txt is the package's one copy of these licence texts, and
About points to it. HarfBuzz's entry has its COPYING, the COPYING of each part
under other licences in its src/ (src/ms-use; any other fails the run), the
headers of the files it compiles that are under another licence (UCDN in
hb-ucd.cc, fasthash in hb-algs.hh), the copyright lines of its files that
COPYING does not list, and the Unicode License V3 for its generated Unicode
tables, which no HarfBuzz file holds: tools/release/licences/Unicode-License-V3.txt.

The compiled files of HarfBuzz, RmlUi and FreeType are read for licence grants
other than their library's own (GRANTS): each must be known (HARFBUZZ_,
RMLUI_ and FREETYPE_FILE_LICENCES, with its text or why none is needed), or the
run fails. RmlUi's entry so has Include/RmlUi/Core/Containers/LICENSE.txt
(robin_hood, itlib), FreeType's the BDF and PCF drivers' licences and the one
of its hash functions. Dear ImGui's has the licences of the two fonts in
imgui_draw.cpp and of the stb parts it keeps.

Code under a licence GPL-3.0 cannot take in must not ship (16.5). Every run,
--check included, reads the tracked text files under src/ and data/ (what the
programs are built from and what the package carries) and fails on an SPDX tag
whose only choice is the version-2-only GPL, or a copyright line of the Linux
kernel. --licences runs this scan alone; it needs no build files.

--check only checks: every linked library is known, every licence file is
there, and no shipped source has such a licence. --self-test checks the scan
on samples. Nothing here downloads anything.
"""

import argparse
import fnmatch
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import textwrap

ROOT = pathlib.Path(__file__).resolve().parents[2]
REPOSITORY = "https://github.com/DeroXP/boiii-free"
DEFAULT_THIRD_PARTY = pathlib.Path(r"C:\Users\Ginld\XercesDev\third_party")
DEFAULT_DOWNLOADS = pathlib.Path(r"C:\Users\Ginld\XercesDev\downloads")

# The shipped programs: premake project -> file name.
PROGRAMS = {"client": "boiii.exe", "xerces_launcher": "Xerces.exe", "xerces_cli": "xerces_cli.exe"}
# Linked libraries that are Xerces' or upstream's own code (GPL-3.0, LICENSE).
OWN_LIBS = {"common", "xerces_core", "tlsdll"}
# Source folders and the programs their code ends up in, for header-only libraries.
SOURCE_DIRS = {
    "src/client": ["boiii.exe"],
    "src/common": ["boiii.exe"],
    "src/xerces_core": ["boiii.exe", "Xerces.exe", "xerces_cli.exe"],
    "src/xerces_launcher": ["Xerces.exe"],
    "src/xerces_cli": ["xerces_cli.exe"],
}


class Problem(Exception):
    pass


# The licence scan (16.5). The tag and the version-2 id are written in two
# parts so that licence scanners reading this file do not take these lines for
# a tag or a notice.
SPDX_TAG = "SPDX-License-" "Identifier:"
SPDX_EXPRESSION = re.compile(re.escape(SPDX_TAG) + r"[ \t]*([^\n]*?)[ \t]*(?:\*/|-->)?[ \t]*$", re.M)
# The version-2-only GPL: the id with or without "-only". "+" and "-or-later"
# reach version 3, and the LGPL ids do not match.
GPL_V2_ONLY = re.compile(r"(?<![\w.-])GPL-" r"2\.0(?:-only)?(?![\w.+-])")
KERNEL_COPYRIGHT = re.compile(r"Copyright\s*(?:\(c\))?[\s\d,-]*Linus\s+" r"Torvalds", re.I)
# Text files the scan reads; fonts, images and other binary data are skipped.
SCANNED_SUFFIXES = {
    ".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".rc", ".asm", ".lua", ".gsc", ".csc", ".gsh",
    ".js", ".html", ".css", ".rml", ".rcss", ".json", ".txt", ".md",
}


def spdx_v2_only(expression: str) -> bool:
    """True when every OR choice of an SPDX expression needs the version-2-only GPL."""
    choices = re.split(r"\s+OR\s+", expression.replace("(", " ").replace(")", " ").strip())
    return all(GPL_V2_ONLY.search(choice) for choice in choices)


def licence_problems_in(text: str) -> list[str]:
    """What in one file's text GPL-3.0 cannot take in, each kind once."""
    found = []
    for match in SPDX_EXPRESSION.finditer(text):
        if spdx_v2_only(match.group(1)):
            found.append(f"SPDX tag '{match.group(1).strip()}' (version 2 only, GPL-3.0 cannot take it in)")
    if KERNEL_COPYRIGHT.search(text):
        found.append("Linux kernel copyright line (kernel code is version 2 only)")
    return list(dict.fromkeys(found))


def licence_problems() -> list[str]:
    """The licence scan over the tracked text files under src/ and data/."""
    listed = subprocess.run(["git", "-c", "core.quotepath=off", "ls-files", "-z", "--", "src", "data"],
                            cwd=ROOT, capture_output=True, text=True, check=True).stdout
    problems = []
    for relative in sorted(p for p in listed.split("\0") if p):
        path = ROOT / relative
        if path.suffix.lower() not in SCANNED_SUFFIXES or not path.is_file():
            continue
        for problem in licence_problems_in(path.read_text(encoding="utf-8", errors="replace")):
            problems.append(f"{relative}: {problem}")
    return problems


def self_test() -> int:
    v2 = "GPL-" "2.0"
    cases = [
        # (text, problems it must give)
        (f"// {SPDX_TAG} {v2}\n", 1),
        (f"/* {SPDX_TAG} {v2}-only */\n", 1),
        (f"// {SPDX_TAG} MIT AND {v2}\n", 1),
        (f"-- {SPDX_TAG} ({v2} OR {v2}-only)\n", 1),
        (" *  Copyright (C) 1991, 1992  Linus " "Torvalds\n", 1),
        (f"// {SPDX_TAG} {v2}\n *  Copyright (C) 1991, 1992  Linus " "Torvalds\n", 2),
        (f"// {SPDX_TAG} {v2}+\n", 0),
        (f"// {SPDX_TAG} {v2}-or-later\n", 0),
        (f"// {SPDX_TAG} {v2} OR MIT\n", 0),
        (f"// {SPDX_TAG} L{v2}-only\n", 0),
        (f"// {SPDX_TAG} LGPL-2.1\n", 0),
        (f"// {SPDX_TAG} GPL-3.0-or-later\n", 0),
        (f"/* {SPDX_TAG} BSD-3-Clause */\n", 0),
        ("// The names follow the Linux kernel's documented strscpy API.\n", 0),
        ("// Copyright (c) 2022 momo5502\n", 0),
    ]
    failures = []
    for text, wanted in cases:
        got = licence_problems_in(text)
        if len(got) != wanted:
            failures.append(f"{text.strip()!r}: gave {got}, wants {wanted} problem(s)")

    # HarfBuzz's texts: COPYING, src/ms-use/COPYING and the Unicode licence, in
    # that order; a licence file under src/ that is not known fails.
    with tempfile.TemporaryDirectory() as temp:
        folder = pathlib.Path(temp) / "harfbuzz-0.0.0"
        (folder / "src/ms-use").mkdir(parents=True)
        (folder / "test").mkdir()
        (folder / "COPYING").write_text("old mit\n", encoding="utf-8")
        (folder / "src/ms-use/COPYING").write_text("microsoft mit\n", encoding="utf-8")
        (folder / "test/COPYING").write_text("not compiled\n", encoding="utf-8")
        try:
            texts = harfbuzz_texts(folder)
            if [text for _, text in texts[:2]] != ["old mit", "microsoft mit"] or len(texts) != 3 \
                    or "UNICODE LICENSE V3" not in texts[2][1]:
                failures.append(f"HarfBuzz texts: {[title for title, _ in texts]}")
        except Problem as problem:
            failures.append(f"HarfBuzz texts: {problem}")
        (folder / "src/other").mkdir()
        (folder / "src/other/LICENSE.txt").write_text("other\n", encoding="utf-8")
        try:
            harfbuzz_texts(folder)
            failures.append("HarfBuzz texts: an unknown src/other/LICENSE.txt passed")
        except Problem:
            pass
    # Licence grants in compiled files: a known one gives its text once, an
    # unknown one fails; holders COPYING does not name are listed.
    with tempfile.TemporaryDirectory() as temp:
        folder = pathlib.Path(temp) / "lib-1.0"
        (folder / "src").mkdir(parents=True)
        old_mit = "/*\n * Copyright (C) 2020  Listed Person\n * Permission is hereby granted, without written " \
                  "agreement and without\n * license or royalty fees, to use\n */\n"
        (folder / "src/main.cc").write_text('#include "a.hh"\n#include "b.hh"\n' + old_mit, encoding="utf-8")
        (folder / "src/a.hh").write_text("/*\n * Copyright (C) 2012 Someone Else\n *\n * Permission to use, copy, "
                                         "modify, and/or distribute this\n * software for any purpose with or "
                                         "without fee\n */\n", encoding="utf-8")
        (folder / "src/b.hh").write_text(old_mit.replace("2020  Listed Person", "2016  New Holder"),
                                         encoding="utf-8")
        (folder / "src/c.hh").write_text("/* Redistribution and use in source and binary forms */\n",
                                         encoding="utf-8")
        files = include_closure(folder, [folder / "src/main.cc"], [folder / "src"])
        if sorted(p.name for p in files) != ["a.hh", "b.hh", "main.cc"]:
            failures.append(f"include closure: {[p.name for p in files]}")
        known = {"src/a.hh": ("a.hh, ISC", lambda f: comment_with(f / "src/a.hh", "permission to use"), "")}
        try:
            texts = file_licence_texts(folder, files, {"Old MIT"}, known)
            if len(texts) != 1 or "Someone Else" not in texts[0][1]:
                failures.append(f"file licences: {texts}")
        except Problem as problem:
            failures.append(f"file licences: {problem}")
        try:
            file_licence_texts(folder, [*files, folder / "src/c.hh"], {"Old MIT"}, known)
            failures.append("file licences: an unknown BSD file passed")
        except Problem:
            pass
        lines = unlisted_copyright_lines(folder, files, "Copyright (C) 2020  Listed Person\n", {"src/a.hh"})
        if lines != ["src/b.hh: Copyright (C) 2016  New Holder"]:
            failures.append(f"copyright lines: {lines}")
    for line in failures:
        print(f"self-test: {line}")
    print(f"notices self-test: {'FAIL' if failures else 'ok'}")
    return 1 if failures else 0


def read(path: pathlib.Path) -> str:
    if not path.is_file():
        raise Problem(f"licence file missing: {path}")
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").strip("\n")


def leading_comment(path: pathlib.Path) -> str:
    """The first /* ... */ block of a source file, without the comment marks."""
    text = read(path)
    match = re.search(r"/\*(.*?)\*/", text, flags=re.S)
    if not match:
        raise Problem(f"no licence comment in {path}")
    lines = [re.sub(r"^\s*\* ?", "", line).rstrip() for line in match.group(1).splitlines()]
    return "\n".join(lines).strip("\n")


def trailing_comment(path: pathlib.Path) -> str:
    """The last /* ... */ block of a source file (stb puts its licence there)."""
    text = read(path)
    blocks = re.findall(r"/\*(.*?)\*/", text, flags=re.S)
    if not blocks:
        raise Problem(f"no licence comment in {path}")
    return blocks[-1].strip("\n")


# HarfBuzz's top-level COPYING says that parts under other licences have a
# COPYING of their own in a subfolder. Of its src/ folder, which the build
# compiles (harfbuzz.cc), only src/ms-use has one; a new one fails the run.
HARFBUZZ_PART_LICENCES = {
    "src/ms-use/COPYING": "src/ms-use/COPYING (Microsoft's Universal Shaping Engine data)",
}
# No file in HarfBuzz's source holds the Unicode licence its generated tables
# come under; this copy of the Unicode License V3 does (unicode.org/license.txt).
UNICODE_LICENCE = ROOT / "tools/release/licences/Unicode-License-V3.txt"
HARFBUZZ_NOTE = (
    "HarfBuzz is under the \"Old MIT\" licence (COPYING, first below). Parts built into "
    "Xerces.exe are under other licences. UCDN (hb-ucd.cc) is under the ISC licence and "
    "fasthash (in hb-algs.hh) under the MIT licence, their headers below; the copyright lines "
    "of HarfBuzz files that COPYING does not list follow them. The Universal Shaping Engine tables "
    "(hb-ot-shaper-use-table.hh, hb-ot-shaper-vowel-constraints.cc) are generated from "
    "Microsoft's data in HarfBuzz's src/ms-use, under the MIT licence of src/ms-use/COPYING. "
    "The tables of character properties, scripts, emoji and shaping classes (hb-ucd-table.hh, "
    "hb-unicode-emoji-table.hh, hb-ot-shaper-use-table.hh, hb-ot-shaper-indic-table.cc, "
    "hb-ot-shaper-arabic-table.hh and others) are generated from the Unicode Character Database "
    "and the Unicode emoji data, (c) Unicode, Inc., under the Unicode License V3 (last below)."
)


# The copy of RmlUi's licence next to the engine file the repository keeps
# (src/xerces_rmlui_harfbuzz/FontFaceHandleHarfBuzz.cpp, from RmlUi's sample).
RMLUI_ENGINE_LICENCE = ROOT / "src/xerces_rmlui_harfbuzz/LICENSE.txt"


def rmlui_engine_licence_matches(rmlui: pathlib.Path) -> None:
    """The repository's copy of RmlUi's licence is the one of the RmlUi being built."""
    if RMLUI_ENGINE_LICENCE.is_file() and read(RMLUI_ENGINE_LICENCE) != read(rmlui / "LICENSE.txt"):
        raise Problem(f"{RMLUI_ENGINE_LICENCE.relative_to(ROOT).as_posix()} is not {rmlui.name}'s LICENSE.txt: "
                      "copy it again")


def harfbuzz_licence_files(folder: pathlib.Path) -> list[str]:
    """The licence files in a HarfBuzz source folder's src/, relative, sorted."""
    names = re.compile(r"^(?:COPYING|LICEN[CS]E)(?:[.-].*)?$", re.I)
    return sorted(p.relative_to(folder).as_posix() for p in (folder / "src").rglob("*")
                  if p.is_file() and names.match(p.name))


def harfbuzz_texts(folder: pathlib.Path) -> list[tuple[str | None, str]]:
    """HarfBuzz's licence texts: COPYING, every part's COPYING under src/, the
    licences in the headers of compiled files that are not HarfBuzz's own, the
    copyright lines COPYING does not list, and the Unicode License V3. A licence
    file under src/, or a compiled file under a licence, this script does not
    know fails."""
    found = harfbuzz_licence_files(folder)
    unknown = [f for f in found if f not in HARFBUZZ_PART_LICENCES]
    if unknown:
        raise Problem(f"{folder}: licence files with no notice (add them to tools/release/notices.py): "
                      + ", ".join(unknown))
    missing = [f for f in HARFBUZZ_PART_LICENCES if f not in found]
    if missing:
        raise Problem(f"{folder}: licence files expected and not there: {', '.join(missing)}")
    copying = read(folder / "COPYING")
    texts: list[tuple[str | None, str]] = [(None, copying)]
    texts += [(title, read(folder / relative)) for relative, title in sorted(HARFBUZZ_PART_LICENCES.items())]
    compiled = include_closure(folder, [folder / "src/harfbuzz.cc"], [folder / "src"])
    texts += file_licence_texts(folder, compiled, {"Old MIT"}, HARFBUZZ_FILE_LICENCES)
    lines = unlisted_copyright_lines(folder, compiled, copying,
                                     skip={p for p, (title, _, _) in HARFBUZZ_FILE_LICENCES.items() if title})
    if lines:
        texts.append(("Copyright lines in HarfBuzz's source files that COPYING does not list "
                      "(the same \"Old MIT\" licence)", "\n".join(lines)))
    texts.append(("Unicode License V3 (the Unicode data in HarfBuzz's generated tables)", read(UNICODE_LICENCE)))
    return texts


# Licence grants, found by their wording in a file's text with the comment marks
# and line breaks folded into single spaces (folded()). A compiled third-party
# file whose text grants a licence other than its library's own must be known
# below (with the text that goes into the notices, or why none is needed), or
# the run fails: a library update that brings in code under another licence
# cannot slip through unnoticed.
GRANTS = {
    "Old MIT": r"without written agreement and without license or royalty fees",
    "MIT": r"permission is hereby granted, free of charge",
    "ISC": r"permission to use, copy, modify, and or distribute this software for any purpose",
    "X11": r"permission to use, copy, modify, distribute, and sell this software",
    "BSD": r"redistribution and use in source and binary forms",
    "Apache": r"licensed under the apache license",
    "Boost": r"boost software license",
    "zlib": r"this software is provided 'as-?is'",
    "public domain": r"public domain",
    "GPL or LGPL": r"gnu (?:lesser )?general public licen[cs]e",
}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".inc"}


def folded(text: str) -> str:
    """Text with comment marks and white space folded into single spaces, lower case."""
    return re.sub(r"[\s*/#;!]+", " ", text).lower()


def grants_in(text: str) -> set[str]:
    flat = folded(text)
    return {name for name, wording in GRANTS.items() if re.search(wording, flat)}


def comment_with(path: pathlib.Path, wording: str) -> str:
    """The first /* ... */ block of a file that has wording (folded), without the comment marks."""
    for match in re.finditer(r"/\*(.*?)\*/", read(path), flags=re.S):
        if wording in folded(match.group(1)):
            lines = [re.sub(r"^\s*\* ?", "", line).rstrip() for line in match.group(1).splitlines()]
            return "\n".join(lines).strip("\n")
    raise Problem(f"no comment with '{wording}' in {path}")


def readme_licence(path: pathlib.Path) -> str:
    """The License part of a FreeType driver's README: from 'License' to 'Credits'."""
    match = re.search(r"^License\n\*+\n(.*?)\n(?:Credits\n\*+|\Z)", read(path), flags=re.S | re.M)
    if not match:
        raise Problem(f"no License part in {path}")
    return match.group(1).strip("\n")


def include_closure(folder: pathlib.Path, sources: list[pathlib.Path],
                    roots: list[pathlib.Path]) -> list[pathlib.Path]:
    """The sources and every file they #include "..." (or <...> under a root), inside folder."""
    seen: set[pathlib.Path] = set()
    todo = [s.resolve() for s in sources if s.is_file()]
    base = folder.resolve()
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        text = path.read_text(encoding="utf-8", errors="replace")
        for name in re.findall(r'^\s*#\s*include\s*["<]([^">]+)[">]', text, flags=re.M):
            for where in [path.parent, *roots]:
                candidate = (where / name).resolve()
                if candidate.is_file() and candidate.is_relative_to(base):
                    todo.append(candidate)
                    break
    return sorted(seen)


def file_licence_texts(folder: pathlib.Path, files: list[pathlib.Path], own: set[str],
                       known: dict[str, tuple[str | None, object, str]]) -> list[tuple[str, str]]:
    """The notices' texts for the files of a library that grant a licence other than its own.

    known: a glob relative to folder -> (title, a function giving the text, why), or
    (None, None, why there is no text to add: covered by another entry, not compiled
    in this configuration, no notice asked). A file under another licence that no
    glob matches fails the run."""
    texts: dict[str, str] = {}
    unknown = []
    for path in files:
        if path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        other = grants_in(path.read_text(encoding="utf-8", errors="replace")) - own
        if not other:
            continue
        relative = path.resolve().relative_to(folder.resolve()).as_posix()
        match = next((v for pattern, v in known.items() if fnmatch.fnmatchcase(relative, pattern)), None)
        if match is None:
            unknown.append(f"{relative} ({', '.join(sorted(other))})")
            continue
        title, text, _ = match
        if title and title not in texts:
            texts[title] = text(folder)
    if unknown:
        raise Problem(f"{folder.name}: compiled files under a licence with no notice "
                      "(add them to tools/release/notices.py): " + ", ".join(unknown))
    return list(texts.items())


def unlisted_copyright_lines(folder: pathlib.Path, files: list[pathlib.Path], copying: str,
                             skip: set[str]) -> list[str]:
    """'file: Copyright line' for the copyright holders in files that copying does not name."""
    def holder(line: str) -> str:
        return re.sub(r"[^a-z]", "", re.sub(r"^.*?copyright\s*(?:©|\(c\))?\s*[\d,\s-]*", "", line,
                                            flags=re.I).lower())
    listed = [holder(line) for line in copying.splitlines() if re.match(r"\s*Copyright\b", line)]
    listed = [h for h in listed if h]
    out = []
    for path in files:
        relative = path.resolve().relative_to(folder.resolve()).as_posix()
        if any(fnmatch.fnmatchcase(relative, pattern) for pattern in skip):
            continue
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = re.search(r"Copyright\s*(?:©|\(C\)|\(c\))?\s*\d{4}[^\n]*", line)
            if not match:
                continue
            text = match.group(0).strip(" *")
            name = holder(text)
            if name and not any(name in h or h in name for h in listed):
                out.append(f"{relative}: {text}")
    return list(dict.fromkeys(out))


# HarfBuzz files built into Xerces.exe whose headers grant another licence.
HARFBUZZ_FILE_LICENCES = {
    "src/hb-ucd.cc": ("src/hb-ucd.cc (UCDN, the Unicode character database lookups), ISC",
                      lambda f: comment_with(f / "src/hb-ucd.cc", "permission to use, copy, modify"), ""),
    "src/hb-algs.hh": ("src/hb-algs.hh (fasthash, by Zilong Tan), MIT",
                       lambda f: comment_with(f / "src/hb-algs.hh", "permission is hereby granted, free of charge"),
                       ""),
    "src/hb-unicode-emoji-table.hh": (None, None, "Unicode data: the Unicode License V3 entry"),
}
# RmlUi Core keeps two libraries of its own under Include/RmlUi/Core/Containers,
# whose licences are in that folder's LICENSE.txt.
RMLUI_FILE_LICENCES = {
    "Include/RmlUi/Core/Containers/*": (
        "Include/RmlUi/Core/Containers/LICENSE.txt (robin_hood and itlib, part of RmlUi Core)",
        lambda f: read(f / "Include/RmlUi/Core/Containers/LICENSE.txt"), ""),
}
# FreeType's LICENSE.TXT names the parts under other licences; this is where
# each one's text is, or why none goes in.
FREETYPE_FILE_LICENCES = {
    "src/bdf/*": ("src/bdf/README (the BDF driver), X11-style",
                  lambda f: readme_licence(f / "src/bdf/README"), ""),
    "src/pcf/pcfutil.c": ("src/pcf/pcfutil.c (part of the PCF driver), X11 licence of The Open Group",
                          lambda f: comment_with(f / "src/pcf/pcfutil.c", "the open group"), ""),
    "src/pcf/*": ("src/pcf/README (the PCF driver), X11-style",
                  lambda f: readme_licence(f / "src/pcf/README"), ""),
    "src/base/fthash.c": ("src/base/fthash.c and include/freetype/internal/fthash.h, X11-style",
                          lambda f: comment_with(f / "src/base/fthash.c", "permission is hereby granted"), ""),
    "include/freetype/internal/fthash.h": (
        "src/base/fthash.c and include/freetype/internal/fthash.h, X11-style",
        lambda f: comment_with(f / "src/base/fthash.c", "permission is hereby granted"), ""),
    "src/gzip/*": (None, None, "zlib's inflate code: the zlib licence, in the zlib entry"),
    "src/base/md5.*": (None, None, "public domain; used in debug builds only"),
    "src/dlg/*": (None, None, "Boost licence; compiled only with FT_DEBUG_LOGGING, which is off"),
    "include/dlg/*": (None, None, "Boost licence; compiled only with FT_DEBUG_LOGGING, which is off"),
    "src/autofit/ft-hb*": (None, None, "HarfBuzz code; compiled only with FT_CONFIG_OPTION_USE_HARFBUZZ, which is off"),
    "src/autofit/hb-script-list.h": (
        None, None, "HarfBuzz code; included only with FT_CONFIG_OPTION_USE_HARFBUZZ, which is off"),
}


def shipped_projects(links: dict[str, set[str]]) -> set[str]:
    """The premake projects whose code is in the shipped programs: theirs and what they link."""
    return set(PROGRAMS) | {lib for libs in links.values() for lib in libs}


def compiled_sources(build: pathlib.Path, folder: pathlib.Path, projects: set[str]) -> list[pathlib.Path]:
    """The files under folder that the ninja files of projects compile (not the spikes')."""
    rule = re.compile(rf"^build \S+: \S+ (\S*[/\\]{re.escape(folder.name)}[/\\]\S+)$", flags=re.M)
    out = set()
    for name in sorted(projects):
        file = build / f"{name}.ninja"
        if not file.is_file():
            continue
        for match in rule.finditer(file.read_text(encoding="utf-8", errors="replace")):
            out.add((build / match.group(1)).resolve())
    return sorted(out)


def rmlui_files(build: pathlib.Path, folder: pathlib.Path, projects: set[str]) -> list[pathlib.Path]:
    """RmlUi's compiled sources, what they include, and its Core headers."""
    headers = [p for p in (folder / "Include/RmlUi/Core").rglob("*") if p.is_file()]
    sources = compiled_sources(build, folder, projects)
    return sorted(set(include_closure(folder, sources, [folder / "Include"])) | {p.resolve() for p in headers})


def freetype_files(build: pathlib.Path, folder: pathlib.Path, projects: set[str]) -> list[pathlib.Path]:
    """FreeType's compiled sources, every file in their folders, and include/."""
    sources = compiled_sources(build, folder, projects)
    files = {p.resolve() for s in sources for p in s.parent.iterdir() if p.is_file()}
    files |= {p.resolve() for p in (folder / "include").rglob("*") if p.is_file()}
    return sorted(files)


# Dear ImGui (boiii.exe) embeds two fonts and three stb libraries.
def imgui_texts(imgui: pathlib.Path) -> list[tuple[str, str]]:
    draw = read(imgui / "imgui_draw.cpp")
    mit = read(imgui / "LICENSE.txt")
    grant = mit[mit.index("Permission is hereby granted"):]
    texts = []
    for font in ("ProggyClean", "ProggyForever"):
        # The section's comment lines, up to its "MIT License / Copyright ..." line.
        match = re.search(rf"\[SECTION\] Default font data \({font}[^)]*\)\n(?://[^\n]*\n)*?"
                          r"// (MIT [Ll]icense / Copyright[^\n]*)", draw)
        if not match:
            raise Problem(f"{imgui / 'imgui_draw.cpp'}: no licence line for {font}")
        holders = match.group(1).split(" / ", 1)[1]
        copyright_lines = "\n".join(c.strip() if c.strip().startswith("Copyright") else "Copyright " + c.strip()
                                    for c in holders.split(", Copyright"))
        texts.append((f"The {font} font, embedded in imgui_draw.cpp, MIT",
                      f"MIT License\n\n{copyright_lines}\n\n{grant}"))
    stb = {name: trailing_comment(imgui / name) for name in ("imstb_rectpack.h", "imstb_textedit.h", "imstb_truetype.h")}
    for text in dict.fromkeys(stb.values()):
        names = ", ".join(n for n, t in stb.items() if t == text)
        texts.append((f"{names} (Sean Barrett's stb libraries, as Dear ImGui keeps them), MIT or public domain",
                      text))
    return texts


def ninja_links(build: pathlib.Path) -> dict[str, set[str]]:
    """Release link inputs per shipped program: library names without bin/ and .lib.

    Libraries the build makes (bin/.../x.lib) and libraries kept in deps/ (the
    prebuilt WebView2 loader) count; other -l names are Windows' own."""
    vendored = {p.stem for p in (ROOT / "deps").rglob("*.lib")}
    out = {}
    for project, exe in PROGRAMS.items():
        file = build / f"{project}.ninja"
        if not file.is_file():
            raise Problem(f"{file} is missing: run premake5 ninja (with the third-party sources) first")
        text = file.read_text(encoding="utf-8", errors="replace")
        match = re.search(rf"^links_{re.escape(project)}_Release_x64 = (.*)$", text, flags=re.M)
        if not match:
            raise Problem(f"no Release link line in {file}")
        libs = set()
        for item in match.group(1).split():
            if item.startswith("-l"):
                name = item[2:].removesuffix(".lib")
                if name in vendored:
                    libs.add(name)
            elif item.endswith(".lib"):
                libs.add(pathlib.PurePosixPath(item).stem)
        out[exe] = libs
    return out


def third_party_dirs(build: pathlib.Path) -> dict[str, pathlib.Path]:
    """The RmlUi, FreeType and HarfBuzz source folders the launcher's build reads, by name."""
    found = {}
    patterns = {"rmlui": r"RmlUi-[^/\\\s]+", "freetype": r"freetype-[^/\\\s]+", "harfbuzz": r"harfbuzz-[^/\\\s]+"}
    for file in sorted(build.glob("*.ninja")):
        text = file.read_text(encoding="utf-8", errors="replace")
        for name, pattern in patterns.items():
            if name in found:
                continue
            match = re.search(rf"(?:-I|\s)(\S*?[/\\]{pattern})[/\\]", text)
            if match:
                found[name] = (build / match.group(1).removeprefix("-I")).resolve()
    return found


def compiled_into(build: pathlib.Path, links: dict[str, set[str]], folder: pathlib.Path) -> list[str]:
    """The programs whose own project, or a library they link, compiles a source file from folder."""
    rule = re.compile(rf"^build \S+: \S+ \S*[/\\]{re.escape(folder.name)}[/\\]", flags=re.M)
    programs = []
    for project, exe in PROGRAMS.items():
        for name in [project, *sorted(links[exe])]:
            file = build / f"{name}.ninja"
            if file.is_file() and rule.search(file.read_text(encoding="utf-8", errors="replace")):
                programs.append(exe)
                break
    return programs


def submodules() -> dict[str, tuple[str, str, str]]:
    """path -> (commit, describe, url) at HEAD."""
    urls = {}
    config = subprocess.run(
        ["git", "config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.(path|url)$"],
        cwd=ROOT, capture_output=True, text=True, check=True,
    ).stdout
    paths = {}
    for line in config.splitlines():
        key, value = line.split(" ", 1)
        name, kind = key[len("submodule."):].rsplit(".", 1)
        (paths if kind == "path" else urls)[name] = value
    url_by_path = {paths[n]: urls.get(n, "") for n in paths}
    out = {}
    tree = subprocess.run(["git", "ls-tree", "-r", "HEAD", "deps"], cwd=ROOT, capture_output=True,
                          text=True, check=True).stdout
    for line in tree.splitlines():
        meta, path = line.split("\t", 1)
        mode, kind, commit = meta.split()
        if kind != "commit":
            continue
        describe = subprocess.run(["git", "-C", path, "describe", "--tags", "--always", commit], cwd=ROOT,
                                  capture_output=True, text=True).stdout.strip()
        out[path] = (commit, describe or commit[:12], url_by_path.get(path, ""))
    return out


def head() -> tuple[str, bool]:
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True,
                            check=True).stdout.strip()
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True,
                           check=True).stdout.strip() != ""
    return commit, dirty


def includes(pattern: str) -> list[str]:
    """The programs whose sources include a header matching pattern."""
    regex = re.compile(rf'^\s*#\s*include\s*[<"]{pattern}', flags=re.M)
    programs = []
    for folder, exes in SOURCE_DIRS.items():
        for path in (ROOT / folder).rglob("*"):
            if path.suffix in {".cpp", ".hpp", ".h", ".c", ".inc"} and path.is_file():
                if regex.search(path.read_text(encoding="utf-8", errors="replace")):
                    programs.extend(e for e in exes if e not in programs)
                    break
    return [p for p in PROGRAMS.values() if p in programs]


def entries(links: dict[str, set[str]], tp: dict[str, pathlib.Path], subs: dict,
            build: pathlib.Path) -> tuple[list[dict], set[str]]:
    """The notices, and the linked libraries they account for."""
    deps = ROOT / "deps"

    def linked(*names: str) -> list[str]:
        return [exe for exe, libs in links.items() if any(n in libs for n in names)]

    def version(path: str) -> str:
        return subs[path][1] if path in subs else ""

    table = [
        # name, licence, programs, source, texts (list of (title or None, text)), note, libs it accounts for
        ("MinHook", "BSD 2-Clause", linked("minhook"), f"deps/minhook ({version('deps/minhook')})",
         [(None, read(deps / "minhook/LICENSE.txt"))],
         "Xerces' thread walk, src/xerces_deps/minhook/minhook_threads.inc, is compiled into MinHook's "
         "hook.c and is under the same licence.", {"minhook"}),
        ("udis86", "BSD 2-Clause", linked("udis86"), f"deps/udis86 ({version('deps/udis86')})",
         [(None, read(deps / "udis86/LICENSE"))], "", {"udis86"}),
        ("AsmJit", "Zlib", linked("asmjit"), f"deps/asmjit ({version('deps/asmjit')})",
         [(None, read(deps / "asmjit/LICENSE.md"))], "", {"asmjit"}),
        ("zlib", "Zlib", linked("zlib"), f"deps/zlib ({version('deps/zlib')})",
         [(None, read(deps / "zlib/LICENSE"))], "", {"zlib"}),
        ("MiniZip", "Zlib", linked("minizip"), "deps/zlib/contrib/minizip",
         [(None, leading_comment(deps / "zlib/contrib/minizip/unzip.h"))], "", {"minizip"}),
        ("LibTomCrypt", "Unlicense", linked("libtomcrypt"), f"deps/libtomcrypt ({version('deps/libtomcrypt')})",
         [(None, read(deps / "libtomcrypt/LICENSE"))], "", {"libtomcrypt"}),
        ("LibTomMath", "Unlicense", linked("libtommath"), f"deps/libtommath ({version('deps/libtommath')})",
         [(None, read(deps / "libtommath/LICENSE"))], "", {"libtommath"}),
        ("curl", "curl licence (MIT-style)", linked("curl"), f"deps/curl ({version('deps/curl')})",
         [(None, read(deps / "curl/COPYING"))], "", {"curl"}),
        ("Brotli", "MIT", linked("brotli"), f"deps/brotli ({version('deps/brotli')})",
         [(None, read(deps / "brotli/LICENSE"))], "", {"brotli"}),
        ("Zstandard", "BSD 3-Clause (zstd is dual-licensed; Xerces uses it under BSD)", linked("zstd"),
         f"deps/zstd ({version('deps/zstd')})", [(None, read(deps / "zstd/LICENSE"))], "", {"zstd"}),
        ("XZ Utils (liblzma)", "BSD Zero Clause", linked("xz"), f"deps/xz ({version('deps/xz')})",
         [(None, read(deps / "xz/COPYING.0BSD"))], "", {"xz"}),
        ("Opus", "BSD 3-Clause", linked("opus"), f"deps/opus ({version('deps/opus')})",
         [(None, read(deps / "opus/COPYING"))], "", {"opus"}),
        ("PortAudio", "PortAudio licence (MIT-style)", linked("portaudio"),
         f"deps/portaudio ({version('deps/portaudio')})", [(None, read(deps / "portaudio/LICENSE.txt"))], "",
         {"portaudio"}),
        ("SQLite", "Public domain (the amalgamation repository: BSD 3-Clause)", linked("sqlite"),
         f"deps/sqlite ({version('deps/sqlite')})", [(None, read(deps / "sqlite/LICENSE"))],
         "SQLite itself is in the public domain; the licence below is the amalgamation repository's.",
         {"sqlite"}),
        ("Dear ImGui", "MIT; embedded fonts MIT; stb parts MIT or public domain", linked("imgui"),
         f"deps/imgui ({version('deps/imgui')})",
         [(None, read(deps / "imgui/LICENSE.txt")), *imgui_texts(deps / "imgui")],
         "imgui_draw.cpp embeds the ProggyClean and ProggyForever fonts, and imstb_rectpack.h, "
         "imstb_textedit.h and imstb_truetype.h are Sean Barrett's stb libraries; their notices "
         "follow Dear ImGui's own.", {"imgui"}),
        ("discord-rpc", "MIT", linked("discord-rpc"), f"deps/discord-rpc ({version('deps/discord-rpc')})",
         [(None, read(deps / "discord-rpc/LICENSE"))], "", {"discord-rpc"}),
        ("hksc (Havok Script compiler, Lua)", "Lua licence (MIT-style)", linked("hksc"),
         f"deps/hksc ({version('deps/hksc')})", [(None, read(deps / "hksc/COPYRIGHT"))], "", {"hksc"}),
        ("WebView2 loader", "BSD 3-Clause (Microsoft)", linked("WebView2LoaderStatic"),
         "deps/Microsoft.Web.WebView2 (vendored, prebuilt WebView2LoaderStatic.lib)",
         [(None, read(deps / "Microsoft.Web.WebView2/LICENSE.txt")),
          ("NOTICE.txt", read(deps / "Microsoft.Web.WebView2/NOTICE.txt"))], "", {"WebView2LoaderStatic"}),
        ("AMD FidelityFX Super Resolution 1.0 (FSR 1)", "MIT", includes(r"upscale_shaders/fsr1_"),
         f"deps/FidelityFX-FSR ({version('deps/FidelityFX-FSR')})",
         [(None, read(deps / "FidelityFX-FSR/license.txt"))],
         "Its EASU and RCAS shaders (ffx_a.h, ffx_fsr1.h) are compiled into "
         "src/client/xerces/upscale_shaders/fsr1_*.h by tools/shaders/build_shaders.py, and the constant "
         "setup in src/xerces_core/upscale.cpp is ported from ffx_fsr1.h.", set()),
        ("RapidJSON", "MIT", includes(r"rapidjson/"), f"deps/rapidjson ({version('deps/rapidjson')})",
         [(None, read(deps / "rapidjson/license.txt"))],
         "Only RapidJSON's include/ folder is compiled; the JSON License the text names covers "
         "bin/jsonchecker, which is not.", set()),
        ("argparse", "MIT", includes(r"argparse"), f"deps/argparse ({version('deps/argparse')})",
         [(None, read(deps / "argparse/LICENSE"))], "", set()),
        ("moodycamel::ConcurrentQueue", "BSD 2-Clause (or Boost 1.0)", includes(r"concurrentqueue"),
         f"deps/concurrentqueue ({version('deps/concurrentqueue')})",
         [(None, read(deps / "concurrentqueue/LICENSE.md"))], "", set()),
        ("frozen", "Apache-2.0", includes(r"frozen/"), f"deps/frozen ({version('deps/frozen')})",
         [(None, read(deps / "frozen/LICENSE"))], "", set()),
        ("gtl", "Apache-2.0", includes(r"gtl/"), f"deps/gtl ({version('deps/gtl')})",
         [(None, read(deps / "gtl/LICENSE")),
          ("licenses/license_folly", read(deps / "gtl/licenses/license_folly")),
          ("licenses/license_lamerman", read(deps / "gtl/licenses/license_lamerman"))], "", set()),
        ("stb_image", "MIT or public domain", includes(r"stb_image\.h"), f"deps/stb ({version('deps/stb')})",
         [(None, read(deps / "stb/LICENSE"))], "", set()),
        ("stb_image_resize2", "MIT or public domain", includes(r"stb_image_resize"),
         f"deps/stb_image_resize ({version('deps/stb_image_resize')})",
         [(None, trailing_comment(deps / "stb_image_resize/stb_image_resize2.h"))], "", set()),
        ("sqlite_orm", "GNU AGPL-3.0", includes(r"sqlite_orm"), f"deps/sqlite_orm ({version('deps/sqlite_orm')})",
         [(None, read(deps / "sqlite_orm/LICENSE"))],
         "GPL-3.0 section 13 allows the combination. If you run a modified boiii.exe that other "
         "players connect to over a network (a dedicated server, or a lobby you host for them), "
         "the AGPL asks you to offer them its source.", set()),
        ("xxHash", "BSD 2-Clause", includes(r"xxhash\.h"), "src/client/launcher/xxhash.h",
         [(None, leading_comment(ROOT / "src/client/launcher/xxhash.h"))], "", set()),
        ("strlcpy (OpenBSD)", "ISC", ["boiii.exe"] if (ROOT / "src/common/str.cpp").is_file() else [],
         "src/common/str.cpp", [(None, leading_comment(ROOT / "src/common/str.cpp"))], "", set()),
    ]
    shipped = shipped_projects(links)
    if "rmlui" in tp:
        rmlui_engine_licence_matches(tp["rmlui"])
        # RmlUi's HarfBuzz font engine (its Samples/basic/harfbuzz, built as
        # xerces_rmlui_harfbuzz with one file copied into src/xerces_rmlui_harfbuzz)
        # is RmlUi's code, under RmlUi's licence.
        engine = linked("xerces_rmlui_harfbuzz")
        table.append(("RmlUi " + tp["rmlui"].name.split("-", 1)[1], "MIT", linked("xerces_rmlui"),
                      f"{tp['rmlui'].name} (outside the repository; SOURCE.txt)",
                      [(None, read(tp["rmlui"] / "LICENSE.txt")),
                       *file_licence_texts(tp["rmlui"], rmlui_files(build, tp["rmlui"], shipped),
                                           set(), RMLUI_FILE_LICENCES)],
                      "RmlUi's HarfBuzz font engine, from RmlUi's Samples/basic/harfbuzz with one file changed "
                      "for Xerces (src/xerces_rmlui_harfbuzz), is part of RmlUi and under the same licence."
                      if engine else "", {"xerces_rmlui", "xerces_rmlui_harfbuzz"}))
    if "freetype" in tp:
        table.append(("FreeType " + tp["freetype"].name.split("-", 1)[1], "FreeType License (FTL)",
                      linked("xerces_freetype"), f"{tp['freetype'].name} (outside the repository; SOURCE.txt)",
                      [(None, read(tp["freetype"] / "LICENSE.TXT")),
                       ("docs/FTL.TXT", read(tp["freetype"] / "docs/FTL.TXT")),
                       *file_licence_texts(tp["freetype"], freetype_files(build, tp["freetype"], shipped),
                                           set(), FREETYPE_FILE_LICENCES)],
                      "Portions of this software are copyright (c) The FreeType Project (www.freetype.org). "
                      "All rights reserved. FreeType is dual-licensed; Xerces uses it under the FreeType "
                      "License. The BDF and PCF drivers and its hash functions come under X11-style licences "
                      "(below, after the FTL); its gzip module is zlib's code, under the zlib licence (the "
                      "zlib entry).", {"xerces_freetype"}))
    # HarfBuzz (launcher/harfbuzz): in whichever project compiles its sources.
    harfbuzz_libs = {lib for libs in links.values() for lib in libs
                     if "harfbuzz" in lib.lower() and lib != "xerces_rmlui_harfbuzz"}
    harfbuzz_in = compiled_into(build, links, tp["harfbuzz"]) if "harfbuzz" in tp else []
    if harfbuzz_in:
        table.append(("HarfBuzz " + tp["harfbuzz"].name.split("-", 1)[1],
                      "Old MIT; parts ISC, MIT and Unicode License V3", harfbuzz_in,
                      f"{tp['harfbuzz'].name} (outside the repository; SOURCE.txt)",
                      harfbuzz_texts(tp["harfbuzz"]), HARFBUZZ_NOTE, harfbuzz_libs))
    fonts = ROOT / "data/xerces/fonts"
    table.append(("Inter Tight and IBM Plex Mono (fonts)", "SIL Open Font License 1.1", ["Xerces.exe"],
                  "data/xerces/fonts (unmodified; the licence texts are next to the fonts)",
                  [(p.name, read(p)) for p in sorted(fonts.glob("OFL-*.txt"))], "", set()))

    out = []
    known = set()
    for name, licence, programs, source, texts, note, libs in table:
        known |= libs
        if not programs:
            continue  # not in this build
        out.append({"name": name, "licence": licence, "programs": programs, "source": source,
                    "texts": texts, "note": note})
    return out, known


def write_text(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # CRLF: these files are read in Notepad on Windows.
    path.write_bytes(text.replace("\n", "\r\n").encode("utf-8"))


def notices_text(items: list[dict], version: str, commit: str) -> str:
    rule = "=" * 78
    lines = [
        f"THIRD-PARTY NOTICES FOR XERCES {version}",
        "",
        "Xerces (Xerces.exe, boiii.exe and xerces_cli.exe) is free software under the",
        "GNU General Public License, version 3: see LICENSE. It is built with the",
        "software below, each under its own licence. Their copyright notices and",
        "licence texts follow, as their licences ask.",
        "",
        "The programs are also linked with the Microsoft C/C++ runtime and the",
        "Windows SDK libraries, system libraries in the sense of GPL-3.0 section 1.",
        "boiii.exe is compiled with declarations from the Steamworks SDK 1.37 headers",
        "(Valve Corporation; deps/SteamworkSDK), which carry no licence file.",
        "",
        f"Generated by tools/release/notices.py from the licence files of commit",
        f"{commit} and the libraries its build links.",
        "",
        "Contents",
        "",
    ]
    for number, item in enumerate(items, 1):
        lines.append(f"  {number:2}. {item['name']} - {item['licence']} - {', '.join(item['programs'])}")
    for number, item in enumerate(items, 1):
        lines += ["", "", rule, f"{number}. {item['name']}", rule, "",
                  f"Licence:  {item['licence']}", f"Used in:  {', '.join(item['programs'])}",
                  f"Source:   {item['source']}"]
        if item["note"]:
            # File names stay whole (hb-ot-shaper-use-table.hh).
            lines += [""] + textwrap.wrap(item["note"], 78, break_on_hyphens=False, break_long_words=False)
        for title, text in item["texts"]:
            lines += [""]
            if title:
                lines += [f"--- {title} ---", ""]
            lines += [text]
    return "\n".join(lines) + "\n"


def testing_hashes() -> dict[str, str]:
    """File name -> SHA-256 from the source table in docs/TESTING.md."""
    out = {}
    text = (ROOT / "docs/TESTING.md").read_text(encoding="utf-8")
    for match in re.finditer(r"^\|\s*`([^`]+)`\s*\|.*\|\s*`([0-9a-f]{64})`\s*\|\s*$", text, flags=re.M):
        out[match.group(1)] = match.group(2)
    return out


# Where each third-party source archive comes from: (archive name, URL) from the version.
ARCHIVES = {
    "rmlui": ("RmlUi", "MIT", lambda v: (f"rmlui-{v}", f"RmlUi-{v}.zip",
                                         f"https://github.com/mikke89/RmlUi/archive/refs/tags/{v}.zip")),
    "freetype": ("FreeType", "FreeType License", lambda v: (
        f"freetype-{v}", f"freetype-{v}.tar.xz",
        f"https://download.savannah.gnu.org/releases/freetype/freetype-{v}.tar.xz")),
    "harfbuzz": ("HarfBuzz", "Old MIT", lambda v: (
        f"harfbuzz-{v}", f"harfbuzz-{v}.tar.xz",
        f"https://github.com/harfbuzz/harfbuzz/releases/download/{v}/harfbuzz-{v}.tar.xz")),
}


def source_text(version: str, commit: str, subs: dict, tp: dict[str, pathlib.Path], used: set[str],
                downloads: pathlib.Path) -> tuple[str, list[str]]:
    warnings = []
    listed = testing_hashes()
    lines = [
        f"SOURCE CODE FOR XERCES {version}",
        "",
        "Xerces is free software under the GNU General Public License, version 3",
        "(LICENSE). This is where the exact source of the programs in this folder is.",
        "",
        "The Xerces repository at the commit this package was built from:",
        "",
        f"  {REPOSITORY}/tree/{commit}",
        "",
        "To get it with the libraries it keeps as Git submodules:",
        "",
        f"  git clone {REPOSITORY}.git xerces",
        f"  git -C xerces checkout {commit}",
        "  git -C xerces submodule update --init --recursive",
        "",
        "Submodules at that commit (folder, commit, repository):",
        "",
    ]
    width = max((len(p) for p in subs), default=0)
    for path in sorted(subs):
        sub_commit, describe, url = subs[path]
        lines.append(f"  {path.ljust(width)}  {sub_commit}  {url}")
    lines += [
        "",
        "Built into Xerces.exe from source archives kept outside the repository",
        "(unpacked into one folder that premake reads with --xerces-third-party):",
    ]
    for key in ("rmlui", "freetype", "harfbuzz"):
        if key not in tp or key not in used:
            continue
        name, licence, where = ARCHIVES[key]
        ver = tp[key].name.split("-", 1)[1]
        folder, archive, url = where(ver)
        file = downloads / folder / archive
        if not file.is_file():
            raise Problem(f"source archive missing: {file}")
        digest = hashlib.sha256(file.read_bytes()).hexdigest()
        if archive in listed and listed[archive] != digest:
            raise Problem(f"{file}: SHA-256 {digest} is not the {listed[archive]} docs/TESTING.md lists")
        if archive not in listed:
            warnings.append(f"{archive} is not in docs/TESTING.md's source table; its hash was read from {file}")
        lines += ["", f"  {name} {ver} ({licence})", f"    {archive}, {file.stat().st_size:,} bytes",
                  f"    SHA-256 {digest}", f"    from {url}"]
    lines += [
        "",
        "How to build: README.md in the repository, section Building. The tools are",
        "LLVM 23 (clang and LLD), Ninja, premake 5.0.0 and the Visual Studio 2022",
        "Build Tools (Windows SDK, MSVC libraries and ATL).",
        "",
    ]
    return "\n".join(lines), warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default=str(ROOT / "build"))
    parser.add_argument("--third-party", default=os.environ.get("XERCES_THIRD_PARTY", str(DEFAULT_THIRD_PARTY)))
    parser.add_argument("--downloads", default=str(DEFAULT_DOWNLOADS))
    parser.add_argument("--notices")
    parser.add_argument("--source")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--licences", action="store_true", help="only the licence scan of src/ and data/")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    build = pathlib.Path(args.build_dir)
    try:
        licences = licence_problems()
        if licences:
            for line in licences:
                print(f"notices: licence problem: {line}")
            raise Problem("the package would ship code GPL-3.0 cannot take in (docs/DESIGN.md 16.5); "
                          "rewrite or remove it first")
        if args.licences:
            print("notices: licence scan of src/ and data/: ok")
            return 0
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        commit, dirty = head()
        links = ninja_links(build)
        tp = third_party_dirs(build)
        subs = submodules()
        items, known = entries(links, tp, subs, build)
        unknown = sorted({lib for libs in links.values() for lib in libs} - known - OWN_LIBS)
        if unknown:
            raise Problem("linked libraries with no notice (add them to tools/release/notices.py): "
                          + ", ".join(unknown))
        used = {k for k in ("rmlui", "freetype") if k in tp}
        if "harfbuzz" in tp and compiled_into(build, links, tp["harfbuzz"]):
            used.add("harfbuzz")
        if args.check:
            print(f"notices: {len(items)} entries, every linked library known: ok")
            return 0
        if args.notices:
            write_text(pathlib.Path(args.notices), notices_text(items, version, commit))
            print(f"wrote {args.notices} ({len(items)} entries)")
        if args.source:
            if dirty:
                print("warning: the tree has changes; SOURCE.txt names the commit, which does not hold them")
            text, warnings = source_text(version, commit, subs, tp, used, pathlib.Path(args.downloads))
            for warning in warnings:
                print(f"warning: {warning}")
            write_text(pathlib.Path(args.source), text)
            print(f"wrote {args.source}")
    except Problem as problem:
        print(f"notices: {problem}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
