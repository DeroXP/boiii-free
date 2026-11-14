#!/usr/bin/env python3
"""How long a mouse movement takes to reach the game's own mouse counters (docs/research/engine-re.md, Threads).

  python -I tools/dev/re/input_latency.py [samples]
  python -I tools/dev/re/input_latency.py --flood <seconds>   moves as fast as it can, for the cost of many

Only while the game's window is in front: sends a relative mouse move of one count (+1, then -1, so the view
does not drift) with SendInput and reads the game's X counter (0x157DF7498, where the Input thread adds each raw
mouse record) from outside until it moves by that count. A read that sees anything else (the game took the sum
for a frame) is not counted. The game's image base comes from its process block, since the client maps the game
into its own process."""
import ctypes
import ctypes.wintypes as wt
import random
import statistics
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
k32.OpenProcess.restype = ctypes.c_void_p
COUNTER_X = 0x157DF7498 - 0x140000000


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long), ("mouseData", wt.DWORD), ("dwFlags", wt.DWORD),
                ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def find_game():
    import subprocess
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq boiii.exe", "/FO", "CSV", "/NH"], capture_output=True,
                         text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower().startswith("boiii"):
            return int(parts[1])
    return None


def flood(seconds, pid):
    """Moves as fast as this process can send them (alternating), for a load like a mouse that reports often."""
    sent = 0
    direction = 1
    end = time.perf_counter() + seconds
    owner = wt.DWORD()
    while time.perf_counter() < end:
        if sent % 64 == 0:
            # only into the game: not a desktop the game has lost the front of
            u32.GetWindowThreadProcessId(u32.GetForegroundWindow(), ctypes.byref(owner))
            if owner.value != pid:
                print("the game is not in front: stopped")
                break
        move = INPUT(type=0)
        move.mi = MOUSEINPUT(direction, 0, 0, 0x0001, 0, None)
        u32.SendInput(1, ctypes.byref(move), ctypes.sizeof(INPUT))
        direction = -direction
        sent += 1
        # a busy wait of about 125 us between moves: about an 8 kHz mouse at best
        until = time.perf_counter() + 0.000125
        while time.perf_counter() < until:
            pass
    print(f"flood: {sent} moves in {seconds} s ({sent / seconds:.0f} a second)")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--flood":
        return flood(float(sys.argv[2]), find_game()) or 0
    samples = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    pid = find_game()
    if not pid:
        print("no game")
        return 1
    process = k32.OpenProcess(0x0010 | 0x0400, False, pid)
    info = (ctypes.c_byte * 48)()
    ntdll.NtQueryInformationProcess(ctypes.c_void_p(process), 0, info, 48, None)
    peb = struct.unpack_from("<Q", bytes(info), 8)[0]
    base = ctypes.c_uint64()
    k32.ReadProcessMemory(ctypes.c_void_p(process), ctypes.c_void_p(peb + 0x10), ctypes.byref(base), 8, None)
    counter = base.value + COUNTER_X
    value = ctypes.c_int32()

    def read():
        k32.ReadProcessMemory(ctypes.c_void_p(process), ctypes.c_void_p(counter), ctypes.byref(value), 4, None)
        return value.value

    def foreground():
        window = u32.GetForegroundWindow()
        owner = wt.DWORD()
        u32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        return owner.value == pid

    latencies, skipped, not_front = [], 0, 0
    direction = 1
    for _ in range(samples):
        if not foreground():
            not_front += 1
            time.sleep(0.05)
            continue
        before = read()
        move = INPUT(type=0)
        move.mi = MOUSEINPUT(direction, 0, 0, 0x0001, 0, None)  # MOUSEEVENTF_MOVE, relative
        start = time.perf_counter()
        u32.SendInput(1, ctypes.byref(move), ctypes.sizeof(INPUT))
        while True:
            now = read()
            elapsed = time.perf_counter() - start
            if now != before or elapsed > 0.05:
                break
        if now == before + direction:
            latencies.append(elapsed * 1e6)
        else:
            skipped += 1
        direction = -direction
        time.sleep(random.uniform(0.002, 0.006))
    print(f"game base 0x{base.value:X}; {len(latencies)} timed, {skipped} not counted (the game took the sum, or "
          f"nothing in 50 ms), {not_front} skipped (the game not in front)")
    if latencies:
        s = sorted(latencies)
        print(f"mouse move to the game's counter: mean {statistics.mean(s):.0f} us, median "
              f"{s[len(s) // 2]:.0f} us, p90 {s[int(len(s) * 0.9)]:.0f} us, max {s[-1]:.0f} us")
    return 0


if __name__ == "__main__":
    sys.exit(main())
