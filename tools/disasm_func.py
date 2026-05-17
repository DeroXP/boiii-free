#!/usr/bin/env python3
"""
Disassemble a function from the dumped BO3 memory, walking from a static
address until ret/jmp-out-of-range. Uses capstone.

Usage: python disasm_func.py <static_hex> [max_instrs] [dump_dir]
"""

import sys
import re
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def parse_manifest(dump_dir):
    mf = (dump_dir / "manifest.txt").read_text()
    base = int(re.search(r"BO3 module base \(runtime, ASLR'd\): 0x([0-9A-Fa-f]+)", mf).group(1), 16)
    sections = []
    for m in re.finditer(r"^\s+(\S+)\s+va=0x([0-9A-Fa-f]+)\s+size=0x([0-9A-Fa-f]+)\s+->\s+(\S+)", mf, re.MULTILINE):
        sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
    return base, sections


def main():
    if len(sys.argv) < 2:
        print("usage: disasm_func.py <static_hex> [max_instrs] [dump_dir]")
        return 1
    static = int(sys.argv[1], 16)
    max_instrs = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    dump_dir = Path(sys.argv[3]) if len(sys.argv) > 3 else Path("tools/bo3_dump")

    base, sections = parse_manifest(dump_dir)
    # Find which .text section contains it
    target_runtime = static - 0x140000000 + base
    target_text = None
    for name, va, size, fname in sections:
        sec_va = base + va
        if sec_va <= target_runtime < sec_va + size:
            target_text = (name, va, size, fname, sec_va)
            break
    if not target_text:
        print(f"no section contains static 0x{static:X}")
        return 1
    name, va, size, fname, sec_va = target_text
    section_bytes = (dump_dir / fname).read_bytes()
    off = target_runtime - sec_va

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    print(f"Disasm at static 0x{static:X} (runtime 0x{target_runtime:X}) in {name}:")
    print()
    count = 0
    for i in md.disasm(section_bytes[off:off + 4096], target_runtime):
        instr_static = i.address - base + 0x140000000
        print(f"  0x{instr_static:X}: {i.mnemonic:6s} {i.op_str}")
        count += 1
        # Stop on ret or unconditional jmp out
        if i.mnemonic in ("ret", "retf"):
            print("  -- end (ret) --")
            break
        if i.mnemonic == "jmp" and i.op_str.startswith("0x") and "+" not in i.op_str:
            # might be a tail-call; flag but continue if user wants more
            print(f"  -- jmp (possible tail-call) --")
        if count >= max_instrs:
            print(f"  -- max instrs {max_instrs} reached --")
            break
    return 0


if __name__ == "__main__":
    sys.exit(main())
