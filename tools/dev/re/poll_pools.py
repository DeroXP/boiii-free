"""Polls the asset pools of every boiii.exe that runs (read-only, from outside) and keeps the most in use per
process for XModels, meshes, materials and images ({pid: {type: [most in use, pool size]}}); writes <out> as
JSON after every change, and stops when the file <out>.stop exists. Pool layout: docs/research/engine-re.md,
"Asset pools the game finds by slot". tools/dev/session/sweep.ps1 -PollPools runs it beside a sweep.

  python -I tools/dev/re/poll_pools.py <out.json>
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import subprocess
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
TYPES = {4: "xmodel", 5: "xmodelmesh", 6: "material", 9: "image"}


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


def read(h, a, n):
    buf = (ctypes.c_ubyte * n)()
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(ctypes.c_void_p(h), ctypes.c_void_p(a), buf, n, ctypes.byref(got))
    return bytes(buf[: got.value]) if ok else b""


def find_base(h):
    sizes = {}
    a = 0
    mbi = MBI()
    while k32.VirtualQueryEx(ctypes.c_void_p(h), ctypes.c_void_p(a), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        if mbi.State == 0x1000 and mbi.AllocationBase:
            sizes[mbi.AllocationBase] = sizes.get(mbi.AllocationBase, 0) + mbi.RegionSize
        a = (mbi.BaseAddress or 0) + mbi.RegionSize
        if a >= 0x7FFFFFFF0000:
            break
    for b, size in sorted(sizes.items(), key=lambda kv: -kv[1]):
        if 0x1C000000 <= size <= 0x1F000000 and read(h, b, 2) == b"MZ":
            return b
    return None


def pids():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq boiii.exe", "/FO", "CSV", "/NH"], capture_output=True,
                         text=True).stdout
    return [int(line.split('","')[1]) for line in out.splitlines() if line.startswith('"boiii.exe"')]


out = sys.argv[1]
peaks = {}
handles = {}
while not os.path.exists(out + ".stop"):
    changed = False
    for pid in pids():
        if pid not in handles:
            h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
            base = find_base(h) if h else None
            handles[pid] = (h, base)
        h, base = handles[pid]
        if not base:
            handles.pop(pid)
            continue
        raw = read(h, base + 0x1493883F0 - 0x140000000, 10 * 0x20)
        if len(raw) < 10 * 0x20:
            continue
        p = peaks.setdefault(str(pid), {})
        for t, name in TYPES.items():
            _, _, count, _, used = struct.unpack_from("<QIi?3xi", raw, t * 0x20)
            if used > p.get(name, (0, 0))[0]:
                p[name] = (used, count)
                changed = True
    if changed:
        with open(out, "w", encoding="utf-8") as f:
            json.dump(peaks, f, indent=1)
    time.sleep(1)
