"""How often each thread of a process wakes (context switches a second) and the system timer's resolution, read
from outside the process (NtQuerySystemInformation, NtQueryTimerResolution): no code in the game, no thread
stopped. python -I tools/dev/re/wakeups.py <process name> [seconds] [--all]"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

ntdll = ctypes.WinDLL("ntdll")


class UNICODE_STRING(ctypes.Structure):
    _fields_ = [("Length", wt.USHORT), ("MaximumLength", wt.USHORT), ("Buffer", ctypes.c_void_p)]


class CLIENT_ID(ctypes.Structure):
    _fields_ = [("UniqueProcess", ctypes.c_void_p), ("UniqueThread", ctypes.c_void_p)]


class SYSTEM_THREAD_INFORMATION(ctypes.Structure):
    _fields_ = [("KernelTime", ctypes.c_longlong), ("UserTime", ctypes.c_longlong), ("CreateTime", ctypes.c_longlong),
                ("WaitTime", wt.ULONG), ("StartAddress", ctypes.c_void_p), ("ClientId", CLIENT_ID),
                ("Priority", ctypes.c_long), ("BasePriority", ctypes.c_long), ("ContextSwitches", wt.ULONG),
                ("ThreadState", wt.ULONG), ("WaitReason", wt.ULONG)]


class SYSTEM_PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("NextEntryOffset", wt.ULONG), ("NumberOfThreads", wt.ULONG), ("Reserved", ctypes.c_byte * 48),
                ("ImageName", UNICODE_STRING), ("BasePriority", ctypes.c_long), ("UniqueProcessId", ctypes.c_void_p)]


def snapshot(name):
    size = 1 << 22
    while True:
        buf = ctypes.create_string_buffer(size)
        need = wt.ULONG()
        status = ntdll.NtQuerySystemInformation(5, buf, size, ctypes.byref(need))
        if status == 0:
            break
        size *= 2
    base = ctypes.addressof(buf)
    offset = 0
    out = {}
    proc_size = 256  # sizeof(SYSTEM_PROCESS_INFORMATION) on x64
    while True:
        p = SYSTEM_PROCESS_INFORMATION.from_address(base + offset)
        image = ctypes.wstring_at(p.ImageName.Buffer, p.ImageName.Length // 2) if p.ImageName.Buffer else ""
        if image.lower() == name.lower():
            for i in range(p.NumberOfThreads):
                t = SYSTEM_THREAD_INFORMATION.from_address(base + offset + proc_size + i * ctypes.sizeof(
                    SYSTEM_THREAD_INFORMATION))
                out[t.ClientId.UniqueThread or 0] = (t.ContextSwitches, t.KernelTime + t.UserTime, t.Priority)
        if not p.NextEntryOffset:
            break
        offset += p.NextEntryOffset
    return out


name = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
low, high, now = wt.ULONG(), wt.ULONG(), wt.ULONG()
ntdll.NtQueryTimerResolution(ctypes.byref(low), ctypes.byref(high), ctypes.byref(now))
print(f"timer resolution now {now.value / 10000:.4f} ms (coarsest {low.value / 10000:.4f}, finest "
      f"{high.value / 10000:.4f})")
a = snapshot(name)
t0 = time.perf_counter()
time.sleep(seconds)
b = snapshot(name)
gap = time.perf_counter() - t0
rows = []
for tid, (switches, cpu, prio) in b.items():
    s0, c0, _ = a.get(tid, (switches, cpu, prio))
    rows.append(((switches - s0) / gap, (cpu - c0) / (gap * 1e7) * 100, tid, prio))
rows.sort(reverse=True)
print(f"{len(rows)} threads of {name} over {gap:.1f} s: wakes a second, share of a core, priority")
for rate, share, tid, prio in rows if "--all" in sys.argv else rows[:25]:
    print(f"  {tid:>6}  {rate:8.0f}/s  {share:5.1f}%  prio {prio}")
