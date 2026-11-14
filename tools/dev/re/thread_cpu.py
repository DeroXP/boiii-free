#!/usr/bin/env python3
"""Which of a process's threads use the CPU, over time, read from outside (docs/research/engine-re.md, "A map's
load"): every interval it reads each thread's CPU time (GetThreadTimes, nothing stopped) and prints one line with
the threads that used at least a tenth of a core in it, busiest first.

  python -I tools/dev/re/thread_cpu.py <pid> [seconds] [interval ms] [engine base hex]

A thread is named by its description (the name the client passes on from the game's own naming) or, without one,
by its start address (in the engine's addresses when the engine base is given, as threads.txt says it). Each line
starts with the wall-clock time, to line up with other watchers.
"""
import ctypes
import ctypes.wintypes as wt
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from find_thread import description, threads  # noqa: E402

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenThread.restype = ctypes.c_void_p
ntdll = ctypes.WinDLL("ntdll")

THREAD_QUERY_INFORMATION = 0x0040
THREAD_QUERY_LIMITED_INFORMATION = 0x0800


def start_address(tid):
    handle = k32.OpenThread(THREAD_QUERY_INFORMATION, False, tid)
    if not handle:
        return 0
    value = ctypes.c_void_p()
    status = ntdll.NtQueryInformationThread(ctypes.c_void_p(handle), 9, ctypes.byref(value),
                                            ctypes.sizeof(value), None)  # ThreadQuerySetWin32StartAddress
    k32.CloseHandle(ctypes.c_void_p(handle))
    return (value.value or 0) if status == 0 else 0


def cpu_time(tid):
    """The thread's kernel + user time in 100 ns units, or None when it is gone."""
    handle = k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
    if not handle:
        return None
    created, exited, kernel, user = (wt.FILETIME() for _ in range(4))
    ok = k32.GetThreadTimes(ctypes.c_void_p(handle), ctypes.byref(created), ctypes.byref(exited),
                            ctypes.byref(kernel), ctypes.byref(user))
    k32.CloseHandle(ctypes.c_void_p(handle))
    if not ok:
        return None

    def ticks(ft):
        return (ft.dwHighDateTime << 32) | ft.dwLowDateTime

    return ticks(kernel) + ticks(user)


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    pid = int(argv[0])
    seconds = float(argv[1]) if len(argv) > 1 else 30.0
    interval = (float(argv[2]) if len(argv) > 2 else 200.0) / 1000.0
    base = int(argv[3], 16) if len(argv) > 3 else 0

    names = {}

    def name_of(tid):
        if tid not in names or not names[tid]:
            text = description(tid)
            if not text:
                address = start_address(tid)
                if base and address:
                    address = address - base + 0x140000000
                text = f"[{address:#x}]" if address else f"tid {tid}"
            names[tid] = text
        return names[tid]

    last = {}
    t0 = time.perf_counter()
    last_t = t0
    while time.perf_counter() - t0 < seconds:
        time.sleep(interval)
        now = time.perf_counter()
        span = now - last_t
        last_t = now
        busy = []
        for tid in threads(pid):
            value = cpu_time(tid)
            if value is None:
                continue
            if tid in last:
                share = (value - last[tid]) / 1e7 / span
                if share >= 0.1:
                    busy.append((share, tid))
            last[tid] = value
        busy.sort(reverse=True)
        wall = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        listed = "  ".join(f"{name_of(tid)} {share * 100:.0f}%" for share, tid in busy[:8])
        print(f"{wall} {now - t0:6.2f} s  {listed}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
