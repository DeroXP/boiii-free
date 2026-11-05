#!/usr/bin/env python3
"""Propose a signature for an address in a module dump (docs/DESIGN.md 4.5).

The dump is a flat image laid out by RVA (tools/dev/re/dump-module.ps1).
llvm-objdump reads the copy that `tools/dev/re/dump-xrefs.ps1 -Fix` writes next
to it (<dump>.pe). Addresses use the preferred base 0x140000000, as upstream's
symbol tables do.

The pattern leaves out what moves between builds or what the running client
had patched when the dump was taken: rel32 call and jump targets, RIP-relative
displacements, and any jmp whose target lies outside the image (a hook the
client wrote over the original bytes). It grows one instruction at a time until
it matches exactly once in the first .text section, at the address asked for.

  sigmaker.py --dump D.bin --address 0x141423D20 --id db.override_asset \\
      --symbol DB_OverrideAsset --feature multi_mod
  sigmaker.py --dump D.bin --address 0x141423A07 --site-in 0x1414236C0 \\
      --id db.load_xassets.expand_call --symbol "DB_LoadXAssets+0x347"

Prints one entry for compat/signatures.json.
"""

import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

DEFAULT_OBJDUMP = (
    r"C:\Users\Ginld\XercesDev\tools\llvm-23.1.5"
    r"\clang+llvm-23.1.5-x86_64-pc-windows-msvc\bin\llvm-objdump.exe"
)
PREFIXES = {0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0xF0}
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$")
MIN_BYTES = 12
MIN_FIXED = 8
MAX_BYTES = 128


def parse_pe(image):
    if image[:2] != b"MZ":
        sys.exit("the dump does not start with an MZ header")
    nt = struct.unpack_from("<I", image, 0x3C)[0]
    if image[nt : nt + 4] != b"PE\0\0":
        sys.exit("no PE signature in the dump")
    count = struct.unpack_from("<H", image, nt + 6)[0]
    optional_size = struct.unpack_from("<H", image, nt + 20)[0]
    optional = nt + 24
    size_of_image = struct.unpack_from("<I", image, optional + 56)[0]
    checksum = struct.unpack_from("<I", image, optional + 64)[0]
    sections = []
    for i in range(count):
        o = optional + optional_size + i * 40
        name = image[o : o + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size, virtual_address = struct.unpack_from("<II", image, o + 8)
        sections.append((name, virtual_address, virtual_size))
    return checksum, size_of_image, sections


def disassemble(objdump, pe, start, stop):
    out = subprocess.run(
        [objdump, "-d", f"--start-address={start}", f"--stop-address={stop}", str(pe)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    instructions = []
    for line in out.splitlines():
        match = LINE.match(line)
        if match:
            code = bytes.fromhex(match.group(2).replace(" ", ""))
            instructions.append((int(match.group(1), 16), code, match.group(3)))
    return instructions


def modrm_index(code):
    i = 0
    while i < len(code) and code[i] in PREFIXES:
        i += 1
    if i < len(code) and 0x40 <= code[i] <= 0x4F:  # REX
        i += 1
    if i >= len(code):
        return None
    if code[i] == 0xC5:  # two-byte VEX, opcode, ModRM
        return i + 3
    if code[i] == 0xC4:  # three-byte VEX
        return i + 4
    if code[i] == 0x62:  # EVEX
        return i + 5
    if code[i] == 0x0F:
        if i + 1 < len(code) and code[i + 1] in (0x38, 0x3A):
            return i + 3
        return i + 2
    return i + 1


def fixed_bytes(address, code, text, base, image_size):
    """Per byte: True if the pattern keeps it, False for a wildcard."""
    fixed = [True] * len(code)
    i = 0
    while i < len(code) and code[i] in PREFIXES:
        i += 1
    opcode = code[i] if i < len(code) else None
    if opcode in (0xE8, 0xE9) and len(code) - i == 5:
        target = address + len(code) + struct.unpack_from("<i", code, i + 1)[0]
        if opcode == 0xE9 and not base <= target < base + image_size:
            return [False] * len(code)  # a hook: the original bytes are unknown
        return fixed[: i + 1] + [False] * 4
    if (
        opcode == 0x0F
        and i + 1 < len(code)
        and 0x80 <= code[i + 1] <= 0x8F
        and len(code) - i == 6
    ):
        return fixed[: i + 2] + [False] * 4
    if "(%rip)" in text:
        m = modrm_index(code)
        if m is not None and m + 5 <= len(code) and (code[m] & 0xC7) == 0x05:
            for k in range(m + 1, m + 5):
                fixed[k] = False
    return fixed


def build(window, base, image_size):
    pattern = []
    for address, code, text in window:
        for byte, keep in zip(code, fixed_bytes(address, code, text, base, image_size)):
            pattern.append(byte if keep else None)
    return pattern


def find(haystack, pattern, limit=2):
    best_start, best_length, i = 0, 0, 0
    while i < len(pattern):
        if pattern[i] is None:
            i += 1
            continue
        j = i
        while j < len(pattern) and pattern[j] is not None:
            j += 1
        if j - i > best_length:
            best_start, best_length = i, j - i
        i = j
    if best_length == 0:
        return []
    anchor = bytes(pattern[best_start : best_start + best_length])
    hits = []
    position = haystack.find(anchor)
    while position != -1:
        start = position - best_start
        if 0 <= start and start + len(pattern) <= len(haystack):
            if all(p is None or haystack[start + k] == p for k, p in enumerate(pattern)):
                hits.append(start)
                if len(hits) >= limit:
                    break
        position = haystack.find(anchor, position + 1)
    return hits


def text_of(pattern):
    return " ".join("??" if b is None else f"{b:02X}" for b in pattern)


def good_enough(pattern):
    return len(pattern) >= MIN_BYTES and sum(b is not None for b in pattern) >= MIN_FIXED


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dump", required=True, type=Path)
    parser.add_argument("--pe", type=Path, help="default: <dump>.pe")
    parser.add_argument("--objdump", default=DEFAULT_OBJDUMP)
    parser.add_argument("--base", default="0x140000000")
    parser.add_argument("--address", required=True)
    parser.add_argument("--site-in", help="for a site: the start of its function")
    parser.add_argument("--id", required=True)
    parser.add_argument("--symbol", required=True)
    parser.add_argument("--feature", action="append", default=[])
    args = parser.parse_args()

    base = int(args.base, 16)
    address = int(args.address, 16)
    pe = args.pe or Path(str(args.dump) + ".pe")
    if not pe.exists():
        sys.exit(f"{pe} is missing: run tools/dev/re/dump-xrefs.ps1 -Dump {args.dump} -Fix")
    image = args.dump.read_bytes()
    checksum, image_size, sections = parse_pe(image)
    text = next(s for s in sections if s[0] == ".text")  # never the second .text
    text_rva, text_size = text[1], text[2]
    haystack = image[text_rva : text_rva + text_size]
    rva = address - base
    if not text_rva <= rva < text_rva + text_size:
        sys.exit(f"0x{address:X} is not in the first .text section")

    if args.site_in:
        start = int(args.site_in, 16)
        instructions = disassemble(args.objdump, pe, start, address + 64)
        index = next((i for i, insn in enumerate(instructions) if insn[0] == address), None)
        if index is None:
            sys.exit(f"0x{address:X} is not an instruction boundary from 0x{start:X}")
        windows = [
            (back, forward)
            for back in range(0, 16)
            for forward in range(0, 6)
            if index - back >= 0 and index + 1 + forward <= len(instructions)
        ]
        windows.sort(
            key=lambda w: sum(len(c) for _, c, _ in instructions[index - w[0] : index + 1 + w[1]])
        )
        kind = "site"
        candidates = [instructions[index - b : index + 1 + f] for b, f in windows]
    else:
        instructions = disassemble(args.objdump, pe, address, address + MAX_BYTES + 16)
        if not instructions or instructions[0][0] != address:
            sys.exit(f"no instruction at 0x{address:X}")
        kind = "function"
        candidates = [instructions[:n] for n in range(1, len(instructions) + 1)]

    for window in candidates:
        pattern = build(window, base, image_size)
        if len(pattern) > MAX_BYTES:
            break
        if not good_enough(pattern):
            continue
        hits = find(haystack, pattern)
        start_rva = window[0][0] - base
        if len(hits) == 1 and text_rva + hits[0] == start_rva:
            entry = {
                "id": args.id,
                "symbol": args.symbol,
                "kind": kind,
                "section": ".text",
                "patterns": [text_of(pattern)],
                "offset": rva - start_rva,
                "known": {f"0x{checksum:X}": f"0x{rva:X}"},
                "features": args.feature,
            }
            print(json.dumps(entry, indent=2))
            return
    sys.exit(f"no unique pattern of at most {MAX_BYTES} bytes for 0x{address:X}")


if __name__ == "__main__":
    main()
