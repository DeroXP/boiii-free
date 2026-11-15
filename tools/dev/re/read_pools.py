"""Reads the asset pools of a running boiii.exe (s_assetPools at 0x1493883F0, 0x60+ pools of 0x20 bytes:
{void* pool; u32 itemSize; i32 itemCount; bool isSingleton; i32 itemAllocCount at 0x14; AssetLink* freeHead}),
from outside, read-only. Prints each pool that is in use or that Xerces grew, and whether its first block is
the game's own (inside the image).
Usage: python -I tools/dev/re/read_pools.py <pid> [type ...]"""
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
NAMES = {0: "physpreset", 4: "xmodel", 5: "xmodelmesh", 6: "material", 9: "image", 0x2B: "fx"}


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
            return b, size
    return None, 0


base, size = find_base()
want = {int(t, 0) for t in sys.argv[2:]}
raw = read(base + 0x1493883F0 - 0x140000000, 0x70 * 0x20)
for t in range(len(raw) // 0x20):
    pool, item_size, count, single, alloc = struct.unpack_from("<QIi?3xi", raw, t * 0x20)
    if want and t not in want:
        continue
    if not want and alloc == 0:
        continue
    own = base <= pool < base + size
    print("type 0x%02X %-10s size 0x%X  count %6d  used %6d  first block %s%s" % (
        t, NAMES.get(t, ""), item_size, count, alloc, "the game's" if own else "heap",
        "  OVER the game's own" if own and alloc > count else ""))
