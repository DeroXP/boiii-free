"""Where one thread of the game spends its time, sampled from outside the process as a profiler does: the thread is
stopped for a few microseconds, its instruction pointer (and the return address on top of its stack) read, and it
is let go. Nothing is loaded into the game.
  python -I tools/dev/re/sample_thread.py <thread id> [samples] [engine start rva] [--raw <file>]
                                          [--engine-base <hex>] [--scan <bytes>] [--seconds <s>]
--seconds stops after that long even if fewer samples were taken (a map load: give many samples).
--keep sets how many code pointers a stack scan keeps (12 by default).
--engine-base places the game's code for a thread that did not start in it (the main thread): the first line
of `xerces_crash_test threads`. --scan reads that many bytes of the stack (still stopped) and keeps the first
values that point into code, a rough call chain (a stack scan: some are stale).
--raw writes each sample's two places, one line each, for a later grouping by function (sample_report.py).
The game's own code is mapped by the client, not loaded as a module: give the thread's start address's RVA in
BlackOps3.exe (0x212BB10 for the game's context threads) to place it."""
import collections
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
psapi = ctypes.WinDLL("psapi")

THREAD_ALL = 0x0002 | 0x0008 | 0x0040 | 0x0010  # suspend/resume, get context, query
PROCESS_READ = 0x0010 | 0x0400


class CONTEXT(ctypes.Structure):
    _fields_ = [("raw", ctypes.c_byte * 1232)]


def module_table(process):
    modules = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(process, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3)
    out = []
    for handle in modules[: needed.value // ctypes.sizeof(ctypes.c_void_p)]:
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(process, ctypes.c_void_p(handle), name, 260)

        class MODULEINFO(ctypes.Structure):
            _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]

        info = MODULEINFO()
        psapi.GetModuleInformation(process, ctypes.c_void_p(handle), ctypes.byref(info), ctypes.sizeof(info))
        out.append((info.base or 0, info.size, name.value))
    return out


def exports(dll):
    """name for each exported function of a system DLL, by address (system DLLs share their base across
    processes in one boot)."""
    handle = k32.LoadLibraryW(dll)
    if not handle:
        return []
    base = handle
    pe = ctypes.c_uint32.from_address(base + 0x3C).value
    rva, size = struct.unpack_from("<II", ctypes.string_at(base + pe + 24 + 112, 8))
    if not rva:
        return []
    d = ctypes.string_at(base + rva, 40)
    _, _, _, _, _, _, nfuncs, nnames, afuncs, anames, aords = struct.unpack("<IIHHIIIIIII", d)
    funcs = struct.unpack(f"<{nfuncs}I", ctypes.string_at(base + afuncs, 4 * nfuncs))
    names = struct.unpack(f"<{nnames}I", ctypes.string_at(base + anames, 4 * nnames))
    ords = struct.unpack(f"<{nnames}H", ctypes.string_at(base + aords, 2 * nnames))
    out = []
    for n, o in zip(names, ords):
        out.append((base + funcs[o], ctypes.string_at(base + n).decode(errors="replace")))
    return sorted(out)


def main():
    raw_path = None
    given_base = None
    args = list(sys.argv[1:])
    if "--raw" in args:
        at = args.index("--raw")
        raw_path = args[at + 1]
        del args[at:at + 2]
    scan = 0
    if "--scan" in args:
        at = args.index("--scan")
        scan = int(args[at + 1], 0)
        del args[at:at + 2]
    keep = 12
    if "--keep" in args:
        at = args.index("--keep")
        keep = int(args[at + 1])
        del args[at:at + 2]
    seconds = None
    if "--seconds" in args:
        at = args.index("--seconds")
        seconds = float(args[at + 1])
        del args[at:at + 2]
    if "--engine-base" in args:
        at = args.index("--engine-base")
        given_base = int(args[at + 1], 16)
        del args[at:at + 2]
    tid = int(args[0])
    samples = int(args[1]) if len(args) > 1 else 2000
    start_rva = int(args[2], 16) if len(args) > 2 else 0x212BB10
    k32.OpenThread.restype = ctypes.c_void_p
    k32.OpenProcess.restype = ctypes.c_void_p
    k32.LoadLibraryW.restype = ctypes.c_void_p
    thread = k32.OpenThread(THREAD_ALL, False, tid)
    if not thread:
        print("cannot open thread", tid, ctypes.get_last_error())
        return 1
    pid = k32.GetProcessIdOfThread(ctypes.c_void_p(thread))
    process = k32.OpenProcess(PROCESS_READ, False, pid)
    start = ctypes.c_void_p()
    ntdll.NtQueryInformationThread(ctypes.c_void_p(thread), 9, ctypes.byref(start), 8, None)
    engine_base = given_base if given_base is not None else (start.value or 0) - start_rva
    mods = module_table(process)
    named = {}
    for dll in ("ntdll.dll", "win32u.dll", "user32.dll", "kernelbase.dll", "kernel32.dll", "gdi32.dll"):
        named[dll] = exports(dll)

    def place(address):
        if engine_base <= address < engine_base + 0x1D75BC00:
            return f"BlackOps3.exe 0x{address - engine_base + 0x140000000:X}"
        for base, size, name in mods:
            if base <= address < base + size:
                table = named.get(name.lower())
                if table:
                    best = None
                    for a, n in table:
                        if a <= address:
                            best = (a, n)
                        else:
                            break
                    if best:
                        return f"{name}!{best[1]}+0x{address - best[0]:X}"
                return f"{name}+0x{address - base:X}"
        return f"0x{address:X}"

    def code(address):
        """Whether a stack value may be a return address: in the game's code sections or in a loaded module (a
        heuristic: a module's data counts too)."""
        rva = address - engine_base
        if 0 <= rva < 0x1D75BC00:
            # the game's image (inside the client's module): its two code sections only, not its data
            return 0x1000 <= rva < 0x2EFDC00 or 0x1AA6B000 <= rva
        return any(base + 0x1000 <= address < base + size for base, size, _ in mods)

    buf = (ctypes.c_byte * (1232 + 16))()
    aligned = (ctypes.addressof(buf) + 15) & ~15
    leaf = collections.Counter()
    caller = collections.Counter()
    pairs = collections.Counter()
    raw = []
    taken = 0
    t0 = time.perf_counter()
    for _ in range(samples):
        if seconds is not None and time.perf_counter() - t0 >= seconds:
            break
        ctypes.memset(aligned, 0, 1232)
        ctypes.c_uint32.from_address(aligned + 0x30).value = 0x00100001  # CONTEXT_CONTROL (AMD64)
        if k32.SuspendThread(ctypes.c_void_p(thread)) == 0xFFFFFFFF:
            break
        ok = k32.GetThreadContext(ctypes.c_void_p(thread), ctypes.c_void_p(aligned))
        stack = b""
        if ok and scan:
            # read while the thread is still stopped, so the stack is the one the context belongs to
            # in pieces: a shallow stack ends before `scan` bytes, and a read past its end fails whole
            sp = ctypes.c_uint64.from_address(aligned + 0x98).value
            piece = ctypes.create_string_buffer(0x100)
            got = ctypes.c_size_t()
            for at in range(0, scan, 0x100):
                if not k32.ReadProcessMemory(ctypes.c_void_p(process), ctypes.c_void_p(sp + at), piece, 0x100,
                                             ctypes.byref(got)):
                    break
                stack += piece.raw[: got.value]
        k32.ResumeThread(ctypes.c_void_p(thread))
        if ok:
            rip = ctypes.c_uint64.from_address(aligned + 0xF8).value
            rsp = ctypes.c_uint64.from_address(aligned + 0x98).value
            ret = ctypes.c_uint64()
            k32.ReadProcessMemory(ctypes.c_void_p(process), ctypes.c_void_p(rsp), ctypes.byref(ret), 8, None)
            a, b = place(rip), place(ret.value)
            leaf[a.split("+0x")[0] if "!" in a else a] += 1
            caller[b.split("+0x")[0] if "!" in b else b] += 1
            pairs[(a.split("+0x")[0] if "!" in a else a, b)] += 1
            chain = []
            for i in range(0, len(stack) - 7, 8):
                value = struct.unpack_from("<Q", stack, i)[0]
                if code(value):
                    chain.append(place(value))
                    if len(chain) >= keep:
                        break
            raw.append(f"{a}\t{b}" + ("\t" + " < ".join(chain) if scan else ""))
            taken += 1
        time.sleep(0.0007)
    gap = time.perf_counter() - t0
    if raw_path:
        with open(raw_path, "w", encoding="utf8") as f:
            f.write("\n".join(raw) + "\n")
    print(f"thread {tid}: {taken} samples over {gap:.1f} s (engine base 0x{engine_base:X})")
    print("where it was:")
    for k, n in leaf.most_common(15):
        print(f"  {n * 100 / taken:5.1f}%  {k}")
    print("with on top of its stack:")
    for (k, b), n in pairs.most_common(20):
        print(f"  {n * 100 / taken:5.1f}%  {k}  <-  {b}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
