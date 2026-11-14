#!/usr/bin/env python3
"""What the game read and how, from a -xerces-io-trace dump (src/client/xerces/io_trace.cpp).

  python -I tools/dev/re/io_report.py <folder> [from_ms] [to_ms]

The folder holds io-trace.csv and io-files.csv (and threads.txt from `xerces_crash_test threads`, to name the
threads). Prints, for the reads asked for between from_ms and to_ms (ms of the trace): each thread's reads by kind,
size and time to complete; then per second the MB read, the reads asked for, how many were under way at once
(most and mean) and the mean time a read took; then the files read most.
"""
import collections
import csv
import os
import re
import statistics
import sys


def thread_names(folder):
    names = {}
    path = os.path.join(folder, "threads.txt")
    if not os.path.exists(path):
        return names
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"\s+\d+\s+(\d+)\s+(\S.*?)\s+0x[0-9A-F]+\s*$", line)
        if m and m.group(1) != "0":
            names[int(m.group(1))] = m.group(2)
        m = re.match(r"\s+(\d+)\s+(\S.*?)\s+cpu ", line)
        if m and int(m.group(1)) not in names and m.group(2).strip():
            names[int(m.group(1))] = m.group(2).strip()
    return names


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    folder = argv[0]
    lo = float(argv[1]) if len(argv) > 1 else float("-inf")
    hi = float(argv[2]) if len(argv) > 2 else float("inf")
    files = {}
    with open(os.path.join(folder, "io-files.csv"), encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            files[int(row["file"])] = (row["flags"], row["name"])
    reads = []
    with open(os.path.join(folder, "io-trace.csv"), encoding="utf-8") as f:
        for row in csv.DictReader(f):
            asked = float(row["asked_ms"])
            if lo <= asked < hi:
                reads.append({"asked": asked, "done": float(row["done_ms"]), "kind": row["kind"],
                              "thread": int(row["thread"]), "file": int(row["file"]), "offset": int(row["offset"]),
                              "size": int(row["size"]), "bytes": int(row["bytes"]), "failed": row["failed"] == "1"})
    names = thread_names(folder)
    print(f"{len(reads)} reads, {sum(r['bytes'] for r in reads) / 2**20:.0f} MB read")
    by_thread = collections.defaultdict(list)
    for r in reads:
        by_thread[(r["thread"], r["kind"])].append(r)
    print("by thread and kind:")
    for (tid, kind), rs in sorted(by_thread.items(), key=lambda kv: -sum(r["bytes"] for r in kv[1])):
        took = [r["done"] - r["asked"] for r in rs if r["done"] >= 0]
        sizes = collections.Counter(r["size"] for r in rs)
        common = ", ".join(f"{s // 1024} KB x{n}" for s, n in sizes.most_common(3))
        print(f"  {names.get(tid, tid)!s:18} {kind:10} {len(rs):6} reads {sum(r['bytes'] for r in rs) / 2**20:7.1f} MB"
              f"  took ms: median {statistics.median(took) if took else -1:.2f}"
              f" p90 {sorted(took)[int(len(took) * 0.9)] if took else -1:.2f}  sizes: {common}")
    if not reads:
        return 0
    start = int(min(r["asked"] for r in reads) // 1000)
    end = int(max(r["asked"] for r in reads) // 1000) + 1
    print("per second (MB read by reads done in it; asked; under way: most, mean; mean ms a read took):")
    events = []
    for r in reads:
        if r["done"] >= 0:
            events.append((r["asked"], 1))
            events.append((r["done"], -1))
    events.sort()
    for s in range(start, end):
        a, b = s * 1000.0, (s + 1) * 1000.0
        done = [r for r in reads if a <= r["done"] < b]
        asked = [r for r in reads if a <= r["asked"] < b]
        # concurrency: sample every 0.5 ms
        level, most, area, last_t = 0, 0, 0.0, a
        for t, d in events:
            if t < a:
                level += d
                continue
            if t >= b:
                break
            area += level * (t - last_t)
            last_t = t
            level += d
            most = max(most, level)
        area += level * (b - last_t)
        took = [r["done"] - r["asked"] for r in done]
        print(f"  {s:4d} s: {sum(r['bytes'] for r in done) / 2**20:7.1f} MB  asked {len(asked):5}  under way most {most:3}"
              f" mean {area / 1000.0:5.2f}  took {statistics.mean(took) if took else 0:6.2f} ms")
    per_file = collections.Counter()
    for r in reads:
        per_file[r["file"]] += r["bytes"]
    print("files read most:")
    for fi, n in per_file.most_common(12):
        flags, name = files.get(fi, ("?", "?"))
        print(f"  {n / 2**20:7.1f} MB  flags {flags}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
