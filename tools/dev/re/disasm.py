#!/usr/bin/env python3
"""Disassemble a function (or a range) of a module dump with every call and jmp target named from the symbol
tables (docs/research/engine-re.md).

  python -I tools/dev/re/disasm.py 0x141ECC700            the whole function there (.pdata bounds)
  python -I tools/dev/re/disasm.py 0x1421F5D90 --calls     only its calls and jumps out
  python -I tools/dev/re/disasm.py 0x14134C590 0x400       a fixed range
  python -I tools/dev/re/disasm.py --bounds 0x1421F60E1    which function an address is in
  python -I tools/dev/re/disasm.py --callers 0x141ECC700   every call to it, grouped by nearest name
  python -I tools/dev/re/disasm.py --find LobbyHost        names that contain the text

llvm-objdump reads the dump's `.pe` copy (dump-xrefs.ps1 -Fix); set XERCES_RE_OBJDUMP to its path when it is
not on PATH, and XERCES_RE_DUMP to another dump. Names are hints: the nearest known symbol at or below an
address, which is not always the function the address is in.
"""
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re_common as rc  # noqa: E402


def disasm(start, stop, symbols, calls_only):
    pe = rc.DEFAULT_DUMP + ".pe"
    out = subprocess.run([rc.DEFAULT_OBJDUMP, "-d", "--no-show-raw-insn", f"--start-address={start:#x}",
                          f"--stop-address={stop:#x}", pe], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)
        if not m:
            continue
        text = m.group(2).strip()
        target = re.search(r"\b(callq|jmp)\s+0x([0-9a-f]+)", text)
        note = ""
        if target:
            note = "   ; " + symbols.near(int(target.group(2), 16))
        if calls_only and not (text.startswith("callq") or (
                target and target.group(1) == "jmp" and not start <= int(target.group(2), 16) < stop)):
            continue
        print(f"{m.group(1)}: {text}{note}")


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    if argv[0] == "--find":
        for a, n in rc.Symbols().find(argv[1]):
            print(f"{a:#x} {n}")
        return 0
    data = rc.load_dump()
    if argv[0] == "--bounds":
        functions = rc.Functions(data)
        for a in argv[1:]:
            r = functions.containing(int(a, 16))
            print(a, "->", f"{r[0]:#x}..{r[1]:#x} ({r[1] - r[0]:#x} bytes)" if r else "in no .pdata range")
        return 0
    symbols = rc.Symbols()
    if argv[0] == "--callers":
        targets = [int(a, 16) for a in argv[1:]]
        for t, sites in rc.calls_to(data, targets).items():
            groups = collections.Counter(symbols.near(s).split("+")[0] for s in sites)
            print(f"{t:#x} ({symbols.near(t)}): {len(sites)} calls")
            for name, n in groups.most_common():
                print(f"  {n:3}  {name}")
        return 0
    start = int(argv[0], 16)
    calls_only = "--calls" in argv
    rest = [a for a in argv[1:] if not a.startswith("--")]
    if rest:
        stop = start + int(rest[0], 0)
    else:
        r = rc.Functions(data).containing(start)
        if r:
            stop = r[1]
        else:
            # a leaf function has no unwind entry: a short range, read up to its ret
            print("; no .pdata range (a leaf function?): the next 0x60 bytes")
            stop = start + 0x60
    disasm(start, stop, symbols, calls_only)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
