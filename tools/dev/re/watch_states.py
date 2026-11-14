#!/usr/bin/env python3
"""Watch a few of the game's state values from outside while something happens (docs/research/engine-re.md,
"A map's load"): every 2 ms or so it reads them (ReadProcessMemory, nothing stopped) and prints each change with the
time since the start, and the game's exit code if it ends.

  python -I tools/dev/re/watch_states.py <pid> <engine base hex> [seconds]

Watched: local client 0's connection state (0x145359BC8: 9 PRIMED, 10 LOADING, 11 ACTIVE), the server's state
(0x15767AC90, sv.state: 0 dead, 1 loading, 2 game) and its four flags after levelTime, and the screen update's last draw time (0x145778C7C, whose
changes are counted a second rather than printed), with the server's level.time (0x14A1B6C14).
"""
import ctypes
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p

WATCHED = [("client connection", 0x145359BC8), ("server state", 0x15767AC90),
           ("server flags", 0x15767ACA8)]  # sv.isUILevel, isRunnable, allowSelfTick, allowNetPackets
DRAW = 0x145778C7C
LEVEL_TIME = 0x14A1B6C14  # the server's level.time (ms of game time), as GSC gettime() reads it
STATE_NAMES = {0: "DISCONNECTED", 1: "CINEMATIC", 2: "UICINEMATIC", 3: "LOGO", 4: "CHALLENGING", 5: "CONNECTING",
               6: "CONFIRMLOADING", 7: "CONNECTED", 8: "SENDINGDATA", 9: "PRIMED", 10: "LOADING", 11: "ACTIVE"}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    pid = int(argv[0])
    base = int(argv[1], 16)
    seconds = float(argv[2]) if len(argv) > 2 else 40.0
    process = k32.OpenProcess(0x0010 | 0x0400, False, pid)
    if not process:
        print("cannot open process", pid)
        return 1

    def read(address):
        value = ctypes.c_int32()
        got = ctypes.c_size_t()
        ok = k32.ReadProcessMemory(ctypes.c_void_p(process), ctypes.c_void_p(base + address - 0x140000000),
                                   ctypes.byref(value), 4, ctypes.byref(got))
        return value.value if ok else None

    last = {}
    draws = 0
    last_draw = read(DRAW)
    t0 = time.perf_counter()
    second = 0
    while time.perf_counter() - t0 < seconds:
        t = time.perf_counter() - t0
        for name, address in WATCHED:
            value = read(address)
            if value != last.get(name):
                if name == "client connection":
                    label = STATE_NAMES.get(value, str(value))
                elif name == "server flags" and value is not None:
                    flags = [(value >> (8 * i)) & 0xFF for i in range(4)]
                    label = "ui {} runnable {} selftick {} netpackets {}".format(*flags)
                else:
                    label = str(value)
                print(f"{t:7.2f} s  {name}: {label}  (level.time {read(LEVEL_TIME)})", flush=True)
                last[name] = value
        draw = read(DRAW)
        if draw != last_draw:
            draws += 1
            last_draw = draw
        if int(t) > second:
            print(f"{second:4d} s  screen draws {draws}, level.time {read(LEVEL_TIME)}", flush=True)
            draws = 0
            second = int(t)
        code = ctypes.c_uint32()
        if k32.GetExitCodeProcess(ctypes.c_void_p(process), ctypes.byref(code)) and code.value != 259:
            print(f"{t:7.2f} s  the game ended, exit code {code.value:#010x}", flush=True)  # 259 = still active
            return 0
        time.sleep(0.002)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
