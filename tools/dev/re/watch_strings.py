"""Watches the script string hash table of every running boiii.exe, from outside and read-only, for the damage
that hangs the game's string free (0x1412D7970: it walks a bucket's chain for an entry that is not on it).

The table: a pointer at 0x143267000 to 0x10000 entries of 8 bytes, {u32 status_next: next entry in the low 16
bits, status in bits 16-17; u32 str: the string's memory-tree node}. A bucket's chain starts at the entry of
its hash and must come back to it. Strings: gScrMemTreePub (0x14505D2D0) -> node pool, node = 0x1C bytes, the
RefString at the node (u32 refCount:16 user:8 byteLen:8, then the text).

Every 2 s: for each bucket in use, follow the chain; a chain that revisits an entry before it gets back to its
start is broken; it must look the same in three more reads over 1.5 s (a torn read does not). The first time a
game shows one, prints the time, the bucket and the strings on the chain,
and writes them to <out>. Stops when <out>.stop exists.

  python -I tools/dev/re/watch_strings.py <out.txt>
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
TABLE_PTR = 0x143267000
MEMTREE_PUB = 0x14505D2D0
NODE = 0x1C


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


def text_of(h, pool, value):
    raw = read(h, pool + value * NODE, 72)
    if len(raw) < 8:
        return "?"
    data = struct.unpack_from("<I", raw, 0)[0]
    s = raw[4:].split(b"\0")[0]
    return "%r (refs %d)" % (s.decode("latin-1"), data & 0xFFFF)


def check(h, base):
    ptr = read(h, base + TABLE_PTR - 0x140000000, 8)
    pub = read(h, base + MEMTREE_PUB - 0x140000000, 8)
    if len(ptr) < 8 or len(pub) < 8:
        return None
    table_addr = struct.unpack("<Q", ptr)[0]
    pool = struct.unpack("<Q", pub)[0]
    raw = read(h, table_addr, 0x10000 * 8)
    if len(raw) < 0x10000 * 8:
        return None
    entries = struct.unpack("<%dI" % (0x20000), raw)
    broken = []
    for bucket in range(1, 0x10000):
        status_next, value = entries[bucket * 2], entries[bucket * 2 + 1]
        if (status_next >> 16) & 3 == 0 or value == 0:
            continue
        seen = {bucket}
        i = status_next & 0xFFFF
        chain = [bucket]
        while i != bucket:
            if i in seen or len(chain) > 0x10000:
                broken.append((bucket, chain + [i]))
                break
            seen.add(i)
            chain.append(i)
            i = entries[i * 2] & 0xFFFF
    return [(b, [(i, entries[i * 2] >> 16 & 3, text_of(h, pool, entries[i * 2 + 1])) for i in chain[:12]])
            for b, chain in broken[:20]]


out = sys.argv[1]
handles = {}
reported = set()
with open(out, "a", encoding="utf-8") as log:
    log.write("watch started %s\n" % time.strftime("%H:%M:%S"))
    log.flush()
    while not os.path.exists(out + ".stop"):
        for pid in pids():
            if pid not in handles:
                h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
                handles[pid] = (h, find_base(h) if h else None)
            h, base = handles[pid]
            if not base or pid in reported:
                continue
            broken = check(h, base)
            # A torn read (the table copied while the game changes it, as levels load) can look broken; a real
            # break holds still. Report a bucket only when three more reads over 1.5 s show the same chain.
            for _ in range(3):
                if not broken:
                    break
                time.sleep(0.5)
                again = {bucket: chain for bucket, chain in (check(h, base) or [])}
                broken = [(bucket, chain) for bucket, chain in broken if again.get(bucket) == chain]
            if broken:
                reported.add(pid)
                log.write("%s pid %d: %d broken chain(s)\n" % (time.strftime("%H:%M:%S"), pid, len(broken)))
                for bucket, chain in broken:
                    log.write("  bucket %d:\n" % bucket)
                    for i, status, text in chain:
                        log.write("    entry %d status %d %s\n" % (i, status, text))
                log.flush()
                print("BROKEN", pid, flush=True)
        time.sleep(2)
