#!/usr/bin/env python3
"""Print the ID of a thread of a process by its description (the name the client passes on from the game's own
thread naming, docs/CRASH_REPORTS.md): waits until one appears.

  python -I tools/dev/re/find_thread.py <pid> <name> [timeout seconds]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenThread.restype = ctypes.c_void_p
k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    out = []
    more = k32.Thread32First(ctypes.c_void_p(snap), ctypes.byref(entry))
    while more:
        if entry.th32OwnerProcessID == pid:
            out.append(entry.th32ThreadID)
        more = k32.Thread32Next(ctypes.c_void_p(snap), ctypes.byref(entry))
    k32.CloseHandle(ctypes.c_void_p(snap))
    return out


def description(tid):
    handle = k32.OpenThread(0x0800, False, tid)  # THREAD_QUERY_LIMITED_INFORMATION
    if not handle:
        return ""
    text = ctypes.c_wchar_p()
    hr = k32.GetThreadDescription(ctypes.c_void_p(handle), ctypes.byref(text))
    k32.CloseHandle(ctypes.c_void_p(handle))
    if hr < 0 or not text.value:
        return ""
    value = text.value
    k32.LocalFree(text)
    return value


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    pid, name = int(argv[0]), argv[1]
    timeout = float(argv[2]) if len(argv) > 2 else 60.0
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < timeout:
        for tid in threads(pid):
            if description(tid) == name:
                print(tid)
                return 0
        time.sleep(0.05)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
