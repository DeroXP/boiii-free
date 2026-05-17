#!/usr/bin/env python3
"""
For each xref to the autoexec queue head, disassemble the bytes following
and look for a store (mov [reg+disp], imm) where imm contains the
queue-entry-flag bit 0x20000. That instruction is the queue-write we want.
"""

import sys
import re
import struct
from pathlib import Path

QUEUE_STATIC = 0x1432E6000
QUEUE_FLAG_BIT = 0x20000

MOD_RM_VALID = {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D}
REG_NAMES_W = {0x05:"rax", 0x0D:"rcx", 0x15:"rdx", 0x1D:"rbx",
               0x25:"rsp", 0x2D:"rbp", 0x35:"rsi", 0x3D:"rdi"}
REG_NAMES_X = {0x05:"r8", 0x0D:"r9", 0x15:"r10", 0x1D:"r11",
               0x25:"r12", 0x2D:"r13", 0x35:"r14", 0x3D:"r15"}


def parse_manifest(dump_dir):
    mf = (dump_dir / "manifest.txt").read_text()
    base = int(re.search(r"BO3 module base \(runtime, ASLR'd\): 0x([0-9A-Fa-f]+)", mf).group(1), 16)
    sections = []
    for m in re.finditer(r"^\s+(\S+)\s+va=0x([0-9A-Fa-f]+)\s+size=0x([0-9A-Fa-f]+)\s+->\s+(\S+)", mf, re.MULTILINE):
        sections.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
    return base, sections


def find_queue_loads(text_bytes, section_va, target_runtime):
    """Find all instructions that load the queue head pointer via RIP-rel."""
    results = []
    for i in range(len(text_bytes) - 7):
        # Looking for: 48 8B [05|0D|15|1D|...] disp32 = mov r64, [rip+disp32]
        # Or: 48 8D [...] disp32 = lea r64, [rip+disp32]
        if text_bytes[i] != 0x48:
            continue
        opc = text_bytes[i+1]
        if opc not in (0x8B, 0x8D):
            continue
        modrm = text_bytes[i+2]
        if modrm not in MOD_RM_VALID:
            continue
        disp = struct.unpack_from("<i", text_bytes, i+3)[0]
        instr_va = section_va + i
        target = (instr_va + 7) + disp
        if target != target_runtime:
            continue
        kind = "mov" if opc == 0x8B else "lea"
        reg = REG_NAMES_W[modrm]
        results.append((i, instr_va, kind, reg))
    return results


def scan_for_flag_write(text_bytes, start_off, scan_len, reg):
    """
    Scan up to scan_len bytes after start_off for a `mov [reg+disp], imm32`
    where imm32 has bit 0x20000 set. Returns list of (offset_into_text, info).
    """
    # mov [reg+disp32], imm32:
    #   reg=rax: opcode C7 ; modrm 80 ; disp32 ; imm32      ([rax+disp32])
    #   reg=rcx: opcode C7 ; modrm 81 ; disp32 ; imm32      ([rcx+disp32])
    #   reg=rdx: opcode C7 ; modrm 82 ; disp32 ; imm32
    #   reg=rbx: opcode C7 ; modrm 83 ; disp32 ; imm32
    # SIB-form for [reg+rX*scale+disp]:
    #   modrm 84 ; SIB ; disp32 ; imm32
    # Short disp8:
    #   modrm 4? ; disp8 ; imm32
    REG_TO_DISP32 = {"rax":0x80, "rcx":0x81, "rdx":0x82, "rbx":0x83,
                     "rbp":0x85, "rsi":0x86, "rdi":0x87}
    REG_TO_DISP8  = {"rax":0x40, "rcx":0x41, "rdx":0x42, "rbx":0x43,
                     "rbp":0x45, "rsi":0x46, "rdi":0x47}
    REG_TO_NODISP = {"rax":0x00, "rcx":0x01, "rdx":0x02, "rbx":0x03,
                     "rbp":0x05, "rsi":0x06, "rdi":0x07}

    rex_w_prefix = 0  # mov imm32 to memory doesn't need REX.W (32-bit imm)
    hits = []
    end = min(start_off + scan_len, len(text_bytes) - 8)
    for j in range(start_off, end):
        # Check for: C7 modrm [stuff] imm32
        if text_bytes[j] != 0xC7:
            continue
        modrm = text_bytes[j+1]
        # Modrm forms with our target reg:
        info = None
        if reg in REG_TO_DISP32 and modrm == REG_TO_DISP32[reg]:
            # disp32 + imm32 = 4 + 4 = 8 bytes after modrm
            if j + 2 + 4 + 4 > len(text_bytes):
                continue
            disp = struct.unpack_from("<i", text_bytes, j + 2)[0]
            imm = struct.unpack_from("<I", text_bytes, j + 6)[0]
            info = ("disp32", disp, imm)
        elif reg in REG_TO_DISP8 and modrm == REG_TO_DISP8[reg]:
            if j + 2 + 1 + 4 > len(text_bytes):
                continue
            disp = struct.unpack_from("<b", text_bytes, j + 2)[0]
            imm = struct.unpack_from("<I", text_bytes, j + 3)[0]
            info = ("disp8", disp, imm)
        elif reg in REG_TO_NODISP and modrm == REG_TO_NODISP[reg]:
            if j + 2 + 4 > len(text_bytes):
                continue
            imm = struct.unpack_from("<I", text_bytes, j + 2)[0]
            info = ("nodisp", 0, imm)
        elif modrm == 0x84:  # SIB form, [reg + rX*scale + disp32]
            if j + 2 + 1 + 4 + 4 > len(text_bytes):
                continue
            sib = text_bytes[j+2]
            base_reg_idx = sib & 7
            # base register: rax=0, rcx=1, rdx=2, rbx=3, rsp=4, rbp=5, rsi=6, rdi=7
            base_name = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi"][base_reg_idx]
            if base_name != reg:
                continue
            disp = struct.unpack_from("<i", text_bytes, j + 3)[0]
            imm = struct.unpack_from("<I", text_bytes, j + 7)[0]
            info = ("SIB+disp32", disp, imm)

        if info is None:
            continue
        kind, disp, imm = info
        # Capture ALL writes (not just the 0x20000-set assumption);
        # we'll grep the output for likely flag patterns
        hits.append((j, kind, disp, imm))

    return hits


def main():
    dump_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tools/bo3_dump")
    base, sections = parse_manifest(dump_dir)
    target_runtime = QUEUE_STATIC - 0x140000000 + base

    print(f"ASLR base 0x{base:X}, queue runtime 0x{target_runtime:X}")
    print()

    text_sec = next((s for s in sections if s[0] == ".text"), None)
    if not text_sec:
        print("no .text in manifest"); return 1
    name, va, size, fname = text_sec
    section_va = base + va
    text_bytes = (dump_dir / fname).read_bytes()
    print(f"Scanning {name} @ 0x{section_va:X}, {len(text_bytes)/1024/1024:.1f} MB")

    loads = find_queue_loads(text_bytes, section_va, target_runtime)
    print(f"\n{len(loads)} queue-head load sites:")
    for off, va_, kind, reg in loads:
        static = (section_va + off) - base + 0x140000000
        print(f"  off=0x{off:08X}  static=0x{static:X}  {kind} {reg}")

    print(f"\nFor each load, scanning next 96 bytes for 'mov [reg+disp], imm32' with bit 0x20000 set:")
    print()
    for off, va_, kind, reg in loads:
        static = va_ - base + 0x140000000
        # Skip the load instr itself (7 bytes), scan next ~96 bytes
        hits = scan_for_flag_write(text_bytes, off + 7, 96, reg)
        if not hits:
            continue
        print(f"  LOAD @ static 0x{static:X}  ({kind} {reg})")
        for hit_off, hit_kind, disp, imm in hits:
            hit_static = (section_va + hit_off) - base + 0x140000000
            print(f"    +0x{hit_off - off:02X}  static=0x{hit_static:X}  mov dword [{reg}+{hit_kind}={disp}], 0x{imm:08X}  (bit 0x20000 SET)")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
