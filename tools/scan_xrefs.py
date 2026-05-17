#!/usr/bin/env python3
"""
Static scanner: find all RIP-relative x86_64 instructions in BlackOps3.exe
that reference a given target virtual address. Default target = autoexec
queue head 0x1432E6000 (from project_boiii_fork_multimod memory).

Looks for:
  48 8B [mod_rm5] disp32  =>  mov reg64, qword [rip+disp32]   (load)
  4C 8B [mod_rm5] disp32  =>  mov r8-15, qword [rip+disp32]   (load, REX.R)
  48 8D [mod_rm5] disp32  =>  lea reg64, [rip+disp32]         (addr-of)
  4C 8D [mod_rm5] disp32  =>  lea r8-15, [rip+disp32]         (addr-of, REX.R)
  48 89 [mod_rm5] disp32  =>  mov qword [rip+disp32], reg64   (store)
  4C 89 [mod_rm5] disp32  =>  mov [rip+disp32], r8-15         (store, REX.R)
  48 03 [mod_rm5] disp32  =>  add reg64, qword [rip+disp32]
  48 8B 05 .. -> rax,  48 8B 0D -> rcx, 48 8B 15 -> rdx, etc.

mod_rm has top 3 bits = reg field, bottom 5 bits = mod+rm = 0x05 for [rip+disp32].
So the valid mod_rm5 bytes are: 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D.
"""

import sys
import struct
from pathlib import Path

BO3_PATH = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops III\BlackOps3.exe")
DEFAULT_TARGETS = [
    0x1432E6000,   # autoexec queue head
]

# Each entry: (instr_byte_pattern, mnemonic). instr_byte_pattern is 2 bytes
# (REX prefix + opcode). After this comes a modr/m byte we filter, then disp32.
PATTERNS = [
    (b"\x48\x8B", "mov r64,[rip+]"),
    (b"\x4C\x8B", "mov r8-15,[rip+]"),
    (b"\x48\x8D", "lea r64,[rip+]"),
    (b"\x4C\x8D", "lea r8-15,[rip+]"),
    (b"\x48\x89", "mov [rip+],r64"),
    (b"\x4C\x89", "mov [rip+],r8-15"),
    (b"\x48\x03", "add r64,[rip+]"),
    (b"\x48\x39", "cmp [rip+],r64"),
    (b"\x48\x3B", "cmp r64,[rip+]"),
    (b"\x4C\x39", "cmp [rip+],r8-15"),
    (b"\xFF",     "call/jmp [rip+]"),  # /2 = call, /4 = jmp
    (b"\xE8",     "call rel32"),       # absolute via rel32, opcode 1 byte then disp32
    (b"\xE9",     "jmp rel32"),
    (b"\x81",     "or [rip+], imm32"),
]

MOD_RM_VALID = {0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D}
REG_NAMES = {0x05:"rax/r8", 0x0D:"rcx/r9", 0x15:"rdx/r10", 0x1D:"rbx/r11",
             0x25:"rsp/r12", 0x2D:"rbp/r13", 0x35:"rsi/r14", 0x3D:"rdi/r15"}


def parse_pe(data: bytes):
    """Return list of (section_name, virtual_addr, raw_offset, size)."""
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    pe = e_lfanew
    assert data[pe:pe+4] == b"PE\x00\x00", "not a PE"
    file_header_off = pe + 4
    num_sections = struct.unpack_from("<H", data, file_header_off + 2)[0]
    opt_hdr_size = struct.unpack_from("<H", data, file_header_off + 16)[0]
    opt_hdr_off = file_header_off + 20
    image_base = struct.unpack_from("<Q", data, opt_hdr_off + 24)[0]

    section_off = opt_hdr_off + opt_hdr_size
    sections = []
    for i in range(num_sections):
        so = section_off + i * 40
        name = data[so:so+8].rstrip(b"\x00").decode("ascii", "replace")
        virt_size = struct.unpack_from("<I", data, so + 8)[0]
        virt_addr = struct.unpack_from("<I", data, so + 12)[0]
        raw_size = struct.unpack_from("<I", data, so + 16)[0]
        raw_off = struct.unpack_from("<I", data, so + 20)[0]
        sections.append((name, virt_addr, raw_off, min(virt_size, raw_size), image_base))
    return image_base, sections


def file_off_to_va(off, sections, image_base):
    for name, va, raw, size, _base in sections:
        if raw <= off < raw + size:
            return image_base + va + (off - raw)
    return None


def scan_text(data: bytes, sections, image_base, targets):
    """Scan all executable sections (.text, .interpr) for RIP-relative refs."""
    text_sections = [s for s in sections if s[0] in (".text", ".interpr")]
    if not text_sections:
        raise RuntimeError("no executable sections")
    results = []
    target_set = set(targets)
    for text in text_sections:
        results.extend(scan_one_section(data, text, image_base, target_set))
    return results


def scan_one_section(data, text, image_base, target_set):
    results = []
    name, va, raw, size, _base = text
    text_bytes = data[raw:raw + size]
    text_start_va = image_base + va
    print(f"  scanning {name} @ 0x{text_start_va:X}, size 0x{size:X}...")

    for i in range(len(text_bytes) - 7):
        b0 = text_bytes[i]
        b1 = text_bytes[i+1] if i+1 < len(text_bytes) else 0

        # Multi-byte prefixed patterns: REX + opcode
        for pat, name_ in PATTERNS[:10]:
            if len(pat) == 2 and b0 == pat[0] and b1 == pat[1]:
                modrm = text_bytes[i+2]
                if modrm not in MOD_RM_VALID:
                    continue
                if i + 7 >= len(text_bytes):
                    continue
                disp32 = struct.unpack_from("<i", text_bytes, i + 3)[0]
                instr_va = text_start_va + i
                target = (instr_va + 7) + disp32
                if target in target_set:
                    results.append((instr_va, name_, REG_NAMES.get(modrm, "?"),
                                    disp32, target))

        # FF /2 = call [rip+disp32]: opcode FF + modrm 15 (/2 + rm=5) = 0x15
        # FF /4 = jmp  [rip+disp32]: opcode FF + modrm 25 (/4 + rm=5) = 0x25
        if b0 == 0xFF and b1 in (0x15, 0x25):
            if i + 6 >= len(text_bytes):
                continue
            disp32 = struct.unpack_from("<i", text_bytes, i + 2)[0]
            instr_va = text_start_va + i
            target_addr_loc = (instr_va + 6) + disp32  # disp32 points to a qword that holds the target
            # The actual call target is the qword at target_addr_loc -- can't resolve without runtime
            # But we can flag it if the LOCATION matches our target
            if target_addr_loc in target_set:
                kind = "call [rip+]" if b1 == 0x15 else "jmp [rip+]"
                results.append((instr_va, kind, "(indir)", disp32, target_addr_loc))

        # E8 call rel32 / E9 jmp rel32 -- direct, 1-byte opcode
        if b0 in (0xE8, 0xE9):
            if i + 5 > len(text_bytes):
                continue
            disp32 = struct.unpack_from("<i", text_bytes, i + 1)[0]
            instr_va = text_start_va + i
            target = (instr_va + 5) + disp32
            if target in target_set:
                kind = "call rel32" if b0 == 0xE8 else "jmp rel32"
                results.append((instr_va, kind, "-", disp32, target))

    return results


def main():
    if not BO3_PATH.exists():
        print(f"BO3 not found at {BO3_PATH}")
        return 1
    data = BO3_PATH.read_bytes()
    image_base, sections = parse_pe(data)
    print(f"PE image base: 0x{image_base:X}")
    for s in sections:
        print(f"  section {s[0]:8s}  va=0x{s[1]:08X}  raw=0x{s[2]:08X}  size=0x{s[3]:X}")
    print()

    targets = DEFAULT_TARGETS
    if len(sys.argv) > 1:
        targets = [int(x, 16) for x in sys.argv[1:]]
    for t in targets:
        print(f"Scanning for xrefs to 0x{t:X}:")
        results = scan_text(data, sections, image_base, [t])
        if not results:
            print("  (none found)")
            continue
        for va, mnem, reg, disp, tgt in results:
            print(f"  0x{va:016X}  {mnem:24s} {reg:14s}  disp=0x{disp & 0xFFFFFFFF:08X}  -> 0x{tgt:X}")
        print(f"  ({len(results)} matches)")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
