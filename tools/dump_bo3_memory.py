#!/usr/bin/env python3
"""
Dump the live BlackOps3.exe process memory (post-Arxan decryption).
The on-disk .text is encrypted; only the running process has plaintext
instructions. Saves each section to a file.

Usage: python dump_bo3_memory.py [output_dir]
Default output dir: tools/bo3_dump/
"""

import ctypes
import ctypes.wintypes as wt
import sys
import struct
from pathlib import Path

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x0002
TH32CS_SNAPMODULE = 0x0008
TH32CS_SNAPMODULE32 = 0x0010

kernel32 = ctypes.windll.kernel32


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(wt.ULONG)),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", wt.LONG),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_char * 260),
    ]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("th32ModuleID", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("GlblcntUsage", wt.DWORD),
        ("ProccntUsage", wt.DWORD),
        ("modBaseAddr", ctypes.POINTER(wt.BYTE)),
        ("modBaseSize", wt.DWORD),
        ("hModule", wt.HMODULE),
        ("szModule", ctypes.c_char * 256),
        ("szExePath", ctypes.c_char * 260),
    ]


def find_pid(name=b"BlackOps3.exe"):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == -1: return None
    e = PROCESSENTRY32()
    e.dwSize = ctypes.sizeof(PROCESSENTRY32)
    ok = kernel32.Process32First(snap, ctypes.byref(e))
    found = None
    while ok:
        if e.szExeFile.lower() == name.lower():
            found = e.th32ProcessID
            break
        ok = kernel32.Process32Next(snap, ctypes.byref(e))
    kernel32.CloseHandle(snap)
    return found


def find_bo3_module(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == -1: return None
    m = MODULEENTRY32()
    m.dwSize = ctypes.sizeof(MODULEENTRY32)
    ok = kernel32.Module32First(snap, ctypes.byref(m))
    found = None
    while ok:
        if m.szModule.lower() == b"blackops3.exe":
            found = (ctypes.addressof(m.modBaseAddr.contents), m.modBaseSize)
            break
        ok = kernel32.Module32Next(snap, ctypes.byref(m))
    kernel32.CloseHandle(snap)
    return found


def read_memory(hproc, addr, size):
    buf = (ctypes.c_ubyte * size)()
    bytes_read = ctypes.c_size_t(0)
    ok = kernel32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), buf, size,
                                     ctypes.byref(bytes_read))
    if not ok:
        return None
    return bytes(bytearray(buf[:bytes_read.value]))


def parse_pe_sections(header_bytes):
    e_lfanew = struct.unpack_from("<I", header_bytes, 0x3C)[0]
    pe = e_lfanew
    if header_bytes[pe:pe+4] != b"PE\x00\x00":
        return None
    num_sections = struct.unpack_from("<H", header_bytes, pe + 4 + 2)[0]
    opt_hdr_size = struct.unpack_from("<H", header_bytes, pe + 4 + 16)[0]
    sec_off = pe + 4 + 20 + opt_hdr_size
    sections = []
    for i in range(num_sections):
        so = sec_off + i * 40
        name = header_bytes[so:so+8].rstrip(b"\x00").decode("ascii", "replace")
        virt_size = struct.unpack_from("<I", header_bytes, so + 8)[0]
        virt_addr = struct.unpack_from("<I", header_bytes, so + 12)[0]
        sections.append((name, virt_addr, virt_size))
    return sections


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("tools/bo3_dump")
    out_dir.mkdir(parents=True, exist_ok=True)

    # BO3 code runs inside the boiii.exe process (BOIII hosts it). Try
    # both names just in case.
    pid = find_pid(b"boiii.exe") or find_pid(b"BlackOps3.exe")
    if not pid:
        print("boiii.exe / BlackOps3.exe not running. Launch BOIII first.")
        return 1
    print(f"Found process pid={pid}")

    hproc = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                                 False, pid)
    if not hproc:
        print(f"OpenProcess failed (run as Admin? err={kernel32.GetLastError()})")
        return 1
    print(f"Opened process handle: {hproc:#x}")

    info = find_bo3_module(pid)
    if not info:
        print("Could not find BlackOps3.exe module in process")
        return 1
    base, mod_size = info
    print(f"BO3 module base: 0x{base:X}, size: 0x{mod_size:X} ({mod_size/1024/1024:.1f} MB)")

    # Dump PE headers (first 0x1000 bytes) to parse sections
    hdr = read_memory(hproc, base, 0x1000)
    if not hdr:
        print("Failed to read PE header")
        return 1
    sections = parse_pe_sections(hdr)
    if not sections:
        print("Failed to parse PE")
        return 1

    print(f"\nDumping {len(sections)} sections:")
    manifest = []
    for name, va, size in sections:
        sec_addr = base + va
        # Pad to page boundary for clean reads
        read_size = (size + 0xFFF) & ~0xFFF
        print(f"  {name:10s} va=0x{va:08X} size=0x{size:08X}  reading...", end="", flush=True)
        mem = read_memory(hproc, sec_addr, read_size)
        if mem is None:
            print(" FAIL")
            continue
        # Trim back to actual section size
        mem = mem[:size]
        safe_name = name.replace(".", "").replace("/", "_") or "noname"
        out_file = out_dir / f"{safe_name}_va_0x{va:08X}.bin"
        out_file.write_bytes(mem)
        print(f" wrote {len(mem)} bytes -> {out_file.name}")
        manifest.append({"name": name, "va": va, "size": size, "file": out_file.name})

    # Also save PE header for static analyzers
    (out_dir / "pe_header.bin").write_bytes(hdr)

    # Save a manifest
    with (out_dir / "manifest.txt").open("w") as f:
        f.write(f"BO3 module base (runtime, ASLR'd): 0x{base:X}\n")
        f.write(f"BO3 module size: 0x{mod_size:X}\n")
        f.write(f"BO3 link base (static): 0x140000000\n")
        f.write(f"ASLR slide: 0x{base - 0x140000000:X}\n\n")
        f.write("Sections (virtual addresses relative to link base 0x140000000):\n")
        for s in manifest:
            f.write(f"  {s['name']:10s} va=0x{s['va']:08X} size=0x{s['size']:08X} -> {s['file']}\n")
    print(f"\nManifest -> {out_dir / 'manifest.txt'}")

    kernel32.CloseHandle(hproc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
