"""Lists the zones a running boiii.exe has in its zone table (g_zoneNames, 64 x 0x60 bytes at 0x149910B80: name[64],
flags, slot, zoneType at 0x48, state at 0x58) and g_zoneCount (0x14939197C), from outside, read-only.
Usage: python -I tools/dev/re/read_zones.py <pid of boiii.exe>
(compat/zone_memory.json stock_zones; docs/DESIGN.md 5.3 step 5)."""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


h = k32.OpenProcess(0x0010 | 0x0400, False, int(sys.argv[1]))


def read(a, n):
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


base = find_base()
g = lambda addr: base + addr - 0x140000000
count = struct.unpack("<I", read(g(0x14939197C), 4))[0]
table = read(g(0x149910B80), 64 * 0x60)
names = []
for i in range(64):
    e = table[i * 0x60:(i + 1) * 0x60]
    if len(e) < 0x60:
        break
    name = e[:64].split(b"\0", 1)[0].decode("latin-1")
    ztype, state = struct.unpack_from("<i", e, 0x48)[0], struct.unpack_from("<i", e, 0x58)[0]
    if name:
        names.append((i, name, ztype, state))
print("g_zoneCount", count, "named entries", len(names))
for i, n, t, s in names:
    print("%2d %-40s type %d state %d" % (i, n, t, s))
