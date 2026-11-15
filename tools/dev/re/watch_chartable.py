"""Watches the engine's Zombies character-table cache (2026-11-14 night) in a running boiii.exe from outside:
the entry for mode 0 at 0x143EC4E40 (int mode, int state, char *map name, CustomizationTable *table), the string the
name points at, the table's body count (+8), and the mapname dvar (pointer at 0x157736848; its string at +0x28).
Prints a line whenever any of it changes. ReadProcessMemory and VirtualQueryEx (to find the image) only.
Usage: python -I tools/dev/re/watch_chartable.py <pid of boiii.exe> <seconds>
(docs/research/engine-re.md, "The character table after a map change")."""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


pid, seconds = int(sys.argv[1]), float(sys.argv[2])
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)


def read(a, n):
    if not a:
        return b""
    buf = (ctypes.c_ubyte * n)()
    got = ctypes.c_size_t()
    ok = k32.ReadProcessMemory(ctypes.c_void_p(h), ctypes.c_void_p(a), buf, n, ctypes.byref(got))
    return bytes(buf[: got.value]) if ok else b""


def find_base():
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
        if 0x1C000000 <= size <= 0x1F000000 and read(b, 2) == b"MZ":
            return b
    return None


def cstr(a):
    raw = read(a, 64)
    return raw.split(b"\0", 1)[0].decode("latin-1") if raw else "?"


def q(a):
    raw = read(a, 8)
    return struct.unpack("<Q", raw)[0] if len(raw) == 8 else 0


t0 = time.perf_counter()
base = None
while base is None and time.perf_counter() - t0 < 30:
    base = find_base()
    time.sleep(0.2)
if not base:
    print("no image")
    sys.exit(1)
g = lambda addr: base + addr - 0x140000000
print("base", hex(base))
last = None
while time.perf_counter() - t0 < seconds:
    entry = read(g(0x143EC4E40), 24)
    if len(entry) == 24:
        mode, state, name, table = struct.unpack("<iiQQ", entry)
        count = struct.unpack("<I", read(table + 8, 4) or b"\0\0\0\0")[0] if table else 0
        dvar = q(g(0x157736848))
        value = q(dvar + 0x28) if dvar else 0
        now = (mode, state, name, cstr(name) if name else "", table, count, value, cstr(value) if value else "")
        if now != last:
            print("%7.2f  entry: mode %d state %d name %#x '%s' table %#x count %d | mapname %#x '%s'%s" % (
                time.perf_counter() - t0, now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7],
                "  SAME BUFFER" if name and name == value else ""))
            sys.stdout.flush()
            last = now
    time.sleep(0.05)
