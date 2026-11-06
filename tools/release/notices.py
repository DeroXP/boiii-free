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
import hashlib
import os
import pathlib
import re
import subprocess
import sys
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
        ("Dear ImGui", "MIT", linked("imgui"), f"deps/imgui ({version('deps/imgui')})",
         [(None, read(deps / "imgui/LICENSE.txt"))], "", {"imgui"}),
        ("discord-rpc", "MIT", linked("discord-rpc"), f"deps/discord-rpc ({version('deps/discord-rpc')})",
         [(None, read(deps / "discord-rpc/LICENSE"))], "", {"discord-rpc"}),
        ("hksc (Havok Script compiler, Lua)", "Lua licence (MIT-style)", linked("hksc"),
         f"deps/hksc ({version('deps/hksc')})", [(None, read(deps / "hksc/COPYRIGHT"))], "", {"hksc"}),
        ("WebView2 loader", "BSD 3-Clause (Microsoft)", linked("WebView2LoaderStatic"),
         "deps/Microsoft.Web.WebView2 (vendored, prebuilt WebView2LoaderStatic.lib)",
         [(None, read(deps / "Microsoft.Web.WebView2/LICENSE.txt")),
          ("NOTICE.txt", read(deps / "Microsoft.Web.WebView2/NOTICE.txt"))], "", {"WebView2LoaderStatic"}),
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
         "GPL-3.0 section 13 allows the combination. If you run a modified dedicated server for "
         "others, the AGPL asks you to offer its source to them.", set()),
        ("xxHash", "BSD 2-Clause", includes(r"xxhash\.h"), "src/client/launcher/xxhash.h",
         [(None, leading_comment(ROOT / "src/client/launcher/xxhash.h"))], "", set()),
        ("strlcpy (OpenBSD)", "ISC", ["boiii.exe"] if (ROOT / "src/common/str.cpp").is_file() else [],
         "src/common/str.cpp", [(None, leading_comment(ROOT / "src/common/str.cpp"))], "", set()),
    ]
    if "rmlui" in tp:
        table.append(("RmlUi " + tp["rmlui"].name.split("-", 1)[1], "MIT", linked("xerces_rmlui"),
                      f"{tp['rmlui'].name} (outside the repository; SOURCE.txt)",
                      [(None, read(tp["rmlui"] / "LICENSE.txt"))], "", {"xerces_rmlui"}))
    if "freetype" in tp:
        table.append(("FreeType " + tp["freetype"].name.split("-", 1)[1], "FreeType License (FTL)",
                      linked("xerces_freetype"), f"{tp['freetype'].name} (outside the repository; SOURCE.txt)",
                      [(None, read(tp["freetype"] / "LICENSE.TXT")),
                       ("docs/FTL.TXT", read(tp["freetype"] / "docs/FTL.TXT"))],
                      "Portions of this software are copyright (c) The FreeType Project (www.freetype.org). "
                      "All rights reserved. FreeType is dual-licensed; Xerces uses it under the FreeType "
                      "License.", {"xerces_freetype"}))
    # HarfBuzz (launcher/harfbuzz): in whichever project compiles its sources.
    harfbuzz_libs = {lib for libs in links.values() for lib in libs if "harfbuzz" in lib.lower()}
    harfbuzz_in = compiled_into(build, links, tp["harfbuzz"]) if "harfbuzz" in tp else []
    if harfbuzz_in:
        table.append(("HarfBuzz " + tp["harfbuzz"].name.split("-", 1)[1], "Old MIT", harfbuzz_in,
                      f"{tp['harfbuzz'].name} (outside the repository; SOURCE.txt)",
                      [(None, read(tp["harfbuzz"] / "COPYING"))], "", harfbuzz_libs))
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
            lines += [""] + textwrap.wrap(item["note"], 78)
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
