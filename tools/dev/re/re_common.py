"""Shared helpers for the offline RE tools over a module dump (docs/DESIGN.md 4.5, docs/research/engine-re.md).

The dump is a flat image laid out by RVA (dump-module.ps1); its `.pe` copy (dump-xrefs.ps1 -Fix) is what
llvm-objdump reads. Addresses are the ones upstream's symbol tables use (preferred base 0x140000000).
"""
import bisect
import glob
import os
import re
import struct

BASE = 0x140000000
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEFAULT_DUMP = os.environ.get(
    "XERCES_RE_DUMP", r"C:\Users\Ginld\XercesDev\dumps\BlackOps3-20261104-223001.bin")
DEFAULT_OBJDUMP = os.environ.get("XERCES_RE_OBJDUMP", "llvm-objdump")


def load_dump(path=DEFAULT_DUMP):
    with open(path, "rb") as f:
        return f.read()


def sections(data):
    """(name, rva, virtual size) of each section, from the dump's own PE header."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional
    out = []
    for i in range(count):
        entry = table + i * 40
        name = data[entry:entry + 8].rstrip(b"\0").decode(errors="replace")
        vsize, rva = struct.unpack_from("<II", data, entry + 8)
        out.append((name, rva, vsize))
    return out


class Functions:
    """Function bounds from .pdata (RUNTIME_FUNCTION entries). The protector splits functions, so one source
    function can be several ranges; a range is still exact for what it covers."""

    def __init__(self, data):
        pdata = next(s for s in sections(data) if s[0] == ".pdata")
        self.ranges = []
        for i in range(pdata[1], pdata[1] + pdata[2] - 11, 12):
            begin, end, _ = struct.unpack_from("<III", data, i)
            if begin or end:
                self.ranges.append((begin, end))
        self.ranges.sort()
        self.starts = [r[0] for r in self.ranges]

    def containing(self, va):
        rva = va - BASE
        i = bisect.bisect_right(self.starts, rva) - 1
        if i >= 0 and self.ranges[i][0] <= rva < self.ranges[i][1]:
            return BASE + self.ranges[i][0], BASE + self.ranges[i][1]
        return None


class Symbols:
    """Names from the client's symbol tables (src/client/game/symbols): the first address of each
    `Name{0x..., 0x..., 0x...}` is the current client's."""

    def __init__(self, root=os.path.join(REPO, "src", "client", "game", "symbols")):
        pat = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\{\s*(0x[0-9A-Fa-f]+)\s*,", re.S)
        self.names = {}
        for path in glob.glob(os.path.join(root, "**", "*.hpp"), recursive=True):
            with open(path, encoding="utf8", errors="replace") as f:
                for m in pat.finditer(f.read()):
                    addr = int(m.group(2), 16)
                    if BASE <= addr < BASE + 0x20000000:
                        self.names.setdefault(addr, m.group(1))
        self.addrs = sorted(self.names)

    def near(self, va):
        """The nearest known name at or below va, as name+offset (only a hint: functions are not all named)."""
        i = bisect.bisect_right(self.addrs, va) - 1
        if i < 0:
            return "?"
        a = self.addrs[i]
        return self.names[a] if a == va else f"{self.names[a]}+{va - a:#x}"

    def find(self, text):
        return sorted((a, n) for a, n in self.names.items() if text.lower() in n.lower())


def calls_to(data, targets, section=".text"):
    """Every E8 (call) / E9 (jmp) rel32 in the section whose target is one of `targets` (addresses).
    A byte scan: confirm each hit by disassembling it."""
    sec = next(s for s in sections(data) if s[0] == section)
    wanted = set(targets)
    hits = {t: [] for t in wanted}
    for i in range(sec[1], sec[1] + sec[2] - 5):
        if data[i] in (0xE8, 0xE9):
            t = BASE + i + 5 + struct.unpack_from("<i", data, i + 1)[0]
            if t in wanted:
                hits[t].append(BASE + i)
    return hits
