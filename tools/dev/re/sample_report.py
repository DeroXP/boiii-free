#!/usr/bin/env python3
"""Group sample_thread.py --raw samples by function (docs/research/engine-re.md).

  python -I tools/dev/re/sample_report.py <raw file> [top]
  python -I tools/dev/re/sample_report.py <raw file> --chain <text in the leaf> [top]

A sample in the game's code counts for the function it is in (.pdata bounds of the dump, named from the
symbol tables when one is known, which is a hint); one in a system DLL counts for that DLL's function, with the
game's function it was called from when the return address on top of the stack is the game's (true at a system
call stub, which is where a waiting thread is). --chain lists, for the samples whose leaf has the text, the
call chains a stack scan found (sample_thread.py --scan), game addresses by function.
"""
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import re_common as rc  # noqa: E402


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    chain_text = None
    if "--chain" in argv:
        at = argv.index("--chain")
        chain_text = argv[at + 1]
        argv = argv[:at] + argv[at + 2:]
    top = int(argv[1]) if len(argv) > 1 else 25
    data = rc.load_dump()
    functions = rc.Functions(data)
    symbols = rc.Symbols()

    def game_function(text):
        m = re.match(r"BlackOps3\.exe 0x([0-9A-F]+)", text)
        if not m:
            return None
        address = int(m.group(1), 16)
        bounds = functions.containing(address)
        if not bounds:
            return f"0x{address:X} (no .pdata range)"
        return f"0x{bounds[0]:X} ({symbols.near(bounds[0])})"

    def short(text):
        in_game = game_function(text)
        if in_game:
            return in_game
        return text.split("+0x")[0] if "!" in text else re.sub(r"\+0x[0-9A-F]+$", "", text)

    groups = collections.Counter()
    chains = collections.Counter()
    total = 0
    matched = 0
    for line in open(argv[0], encoding="utf8"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 2:
            continue
        leaf, above = parts[0], parts[1]
        total += 1
        if chain_text is not None:
            if chain_text in leaf and len(parts) > 2:
                matched += 1
                steps = []
                for step in parts[2].split(" < "):
                    name = short(step)
                    if not steps or steps[-1] != name:
                        steps.append(name)
                chains[" < ".join(steps[:8])] += 1
            continue
        in_game = game_function(leaf)
        if in_game:
            groups[in_game] += 1
            continue
        caller = game_function(above)
        groups[f"{short(leaf)}  <-  {caller}" if caller else short(leaf)] += 1
    if chain_text is not None:
        print(f"{matched} of {total} samples in {chain_text}")
        for key, n in chains.most_common(top):
            print(f"  {n * 100 / max(matched, 1):5.1f}%  {key}")
        return 0
    print(f"{total} samples")
    for key, n in groups.most_common(top):
        print(f"  {n * 100 / total:5.1f}%  {key}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
