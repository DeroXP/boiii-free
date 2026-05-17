#!/usr/bin/env python3
"""
Scan dumped BO3 process memory (post-Arxan decryption) for RIP-relative
references to a target address. Default target = autoexec queue head.

Usage: python scan_dump.py [target_runtime_hex] [dump_dir]
Default dump_dir: tools/bo3_dump/
"""

import sys
import re
import struct
from pathlib import Path

# Default queue head location: needs runtime address.
# Static = 0x1432E6000. Runtime = static - 0x140000000 + ASLR_base.
# We read ASLR_base from manifest.txt.

MOD_RM_VALID = {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D}
REG_NAMES_W = {0x05:"rax", 0x0D:"rcx", 0x15:"rdx", 0x1D:"rbx",
               0x25:"rsp", 0x2D:"rbp", 0x35:"rsi", 0x3D:"rdi"}
REG_NAMES_X = {0x05:"r8",  0x0D:"r9",  0x15:"r10", 0x1D:"r11",
               0x25:"r12", 0x2D:"r13", 0x35:"r14", 0x3D:"r15"}

# Each entry: (rex+opcode_2bytes, friendly_name, register_map)
TWO_BYTE = [
    (b"\x48\x8B", "mov  r64,[rip+]", REG_NAMES_W),  # load
    (b"\x4C\x8B", "mov  rX,[rip+]",  REG_NAMES_X),
    (b"\x48\x8D", "lea  r64,[rip+]", REG_NAMES_W),  # addr-of
    (b"\x4C\x8D", "lea  rX,[rip+]",  REG_NAMES_X),
    (b"\x48\x89", "mov  [rip+],r64", REG_NAMES_W),  # store
    (b"\x4C\x89", "mov  [rip+],rX",  REG_NAMES_X),
    (b"\x48\x39", "cmp  [rip+],r64", REG_NAMES_W),
    (b"\x48\x3B", "cmp  r64,[rip+]", REG_NAMES_W),
    (b"\x48\x03", "add  r64,[rip+]", REG_NAMES_W),
    (b"\x48\x09", "or   [rip+],r64", REG_NAMES_W),
    (b"\x48\x0B", "or   r64,[rip+]", REG_NAMES_W),
    (b"\x48\x21", "and  [rip+],r64", REG_NAMES_W),
    (b"\x48\x23", "and  r64,[rip+]", REG_NAMES_W),
    (b"\x48\x31", "xor  [rip+],r64", REG_NAMES_W),
    (b"\x48\x33", "xor  r64,[rip+]", REG_NAMES_W),
]


def parse_manifest(dump_dir):
    """Read manifest.txt to get ASLR base + section list."""
    mf = (dump_dir / "manifest.txt").read_text()
    base_match = re.search(r"BO3 module base \(runtime, ASLR'd\): 0x([0-9A-Fa-f]+)", mf)
    if not base_match:
        raise RuntimeError("no base in manifest")
    base = int(base_match.group(1), 16)
    sections = []
    for m in re.finditer(r"^\s+(\S+)\s+va=0x([0-9A-Fa-f]+)\s+size=0x([0-9A-Fa-f]+)\s+->\s+(\S+)", mf, re.MULTILINE):
        sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
    return base, sections


def scan_section(bytes_, section_va_runtime, target):
    """Scan for any 2-byte-prefix RIP-relative instruction targeting `target`."""
    results = []
    n = len(bytes_)
    for i in range(n - 7):
        b0 = bytes_[i]
        b1 = bytes_[i+1]
        modrm = bytes_[i+2]
        for pat, name, regmap in TWO_BYTE:
            if pat[0] != b0 or pat[1] != b1:
                continue
            if modrm not in MOD_RM_VALID:
                continue
            disp32 = struct.unpack_from("<i", bytes_, i+3)[0]
            instr_va = section_va_runtime + i
            t = (instr_va + 7) + disp32
            if t == target:
                results.append((instr_va, name, regmap.get(modrm, "?"), disp32))
            break  # found the prefix match; no need to try others
    # Also: E8 call rel32, E9 jmp rel32
    for i in range(n - 5):
        b0 = bytes_[i]
        if b0 in (0xE8, 0xE9):
            disp32 = struct.unpack_from("<i", bytes_, i+1)[0]
            instr_va = section_va_runtime + i
            t = (instr_va + 5) + disp32
            if t == target:
                kind = "call rel32" if b0 == 0xE8 else "jmp  rel32"
                results.append((instr_va, kind, "-", disp32))
    # Also: FF 15 call [rip+disp32], FF 25 jmp [rip+disp32]
    for i in range(n - 6):
        if bytes_[i] == 0xFF and bytes_[i+1] in (0x15, 0x25):
            disp32 = struct.unpack_from("<i", bytes_, i+2)[0]
            instr_va = section_va_runtime + i
            t = (instr_va + 6) + disp32
            if t == target:
                kind = "call [rip+]" if bytes_[i+1] == 0x15 else "jmp  [rip+]"
                results.append((instr_va, kind, "indir", disp32))
    return results


def runtime_to_static(addr, base):
    """Convert runtime addr back to static (link-time) addr."""
    return addr - base + 0x140000000


def main():
    dump_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path("tools/bo3_dump")
    if not dump_dir.exists():
        print(f"dump dir not found: {dump_dir}")
        return 1

    base, sections = parse_manifest(dump_dir)
    print(f"ASLR base: 0x{base:X}")
    print(f"Static -> runtime offset: +0x{base - 0x140000000:X}")
    print()

    # Default target = autoexec queue head static 0x1432E6000
    target_static = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1432E6000
    target_runtime = target_static - 0x140000000 + base
    print(f"Target static: 0x{target_static:X}")
    print(f"Target runtime: 0x{target_runtime:X}")
    print()

    total = 0
    for name, va, size, fname in sections:
        if name not in (".text", ".interpr"):
            continue
        path = dump_dir / fname
        if not path.exists():
            continue
        section_va_runtime = base + va
        bytes_ = path.read_bytes()
        print(f"Scanning {name} @ 0x{section_va_runtime:X} ({len(bytes_)/1024/1024:.1f} MB)...")
        results = scan_section(bytes_, section_va_runtime, target_runtime)
        for instr_va, mnem, reg, disp in results:
            static = runtime_to_static(instr_va, base)
            print(f"  runtime=0x{instr_va:X}  static=0x{static:X}  {mnem:20s} {reg:5s}  disp=0x{disp & 0xFFFFFFFF:08X}")
        total += len(results)
        if results:
            print(f"  ({len(results)} in this section)")
    print(f"\nTotal: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
