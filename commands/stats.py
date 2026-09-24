#!/usr/bin/env python3
"""Transport and switch counters from the simulator output (out.txt of every run).

Usage:
  commands/stats.py RESULTS...        one row per run (an out.txt or any directory above it)
  commands/stats.py RESULTS --csv x   also write the table as CSV

Columns: data packets, retransmissions (% of packets), RTOs, trim NACKs, EVs
retired / restored (SRv6), switch drops / trims / ECN marks, T0->T1 load
imbalance (max/mean bytes over all T0->T1 links), messages, and the wall-clock
time and peak memory of the run (time.txt, written by commands/run.sh).
"""
import argparse
import os
import re

from common import find_files, print_table, run_label

PATTERNS = {
    "packets": r"Senders: (\d+) data packets",
    "retx": r"(\d+) retransmissions",
    "rtos": r"(\d+) RTOs",
    "trimNacks": r"(\d+) trim NACKs",
    "evRetired": r"(\d+) EVs retired",
    "evRestored": r"\((\d+) restored\)",
    "drops": r"Switches: (\d+) drops",
    "trims": r"(\d+) trims",
    "ecnMarks": r"(\d+) ECN marks",
    "imbalance": r"all T0->T1: .* max/mean ([\d.]+)",
    "messages": r"Messages: (\d+)",
}


def parse(path):
    text = open(path).read()
    r = {}
    for key, pattern in PATTERNS.items():
        m = re.search(pattern, text)
        r[key] = (float(m.group(1)) if "." in m.group(1) else int(m.group(1))) if m else "-"
    if "Used time" not in text:
        r["status"] = "running/failed"
    else:
        m = re.search(r"Tasks done: (\d+)/(\d+)", text)
        r["status"] = "ok" if not m or m.group(1) == m.group(2) else f"tasks {m.group(1)}/{m.group(2)}"
    r["wall_s"], r["mem_GB"] = "-", "-"
    time_file = os.path.join(os.path.dirname(path), "time.txt")
    if os.path.exists(time_file):
        m = re.search(r"([\d.]+) s ([\d.]+) KB", open(time_file).read())
        if m:
            r["wall_s"], r["mem_GB"] = float(m.group(1)), float(m.group(2)) / 1e6
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="out.txt files or directories containing them")
    ap.add_argument("--csv", metavar="CSV", help="also write the table as CSV")
    args = ap.parse_args()

    files = find_files(args.paths, "out.txt")
    header = ["run", "status", "packets", "retx", "retx%", "rtos", "trimNacks", "evRetired",
              "evRestored", "drops", "trims", "ecnMarks", "imbalance", "messages", "wall_s", "mem_GB"]
    rows = []
    for path in files:
        r = parse(path)
        pct = 100 * r["retx"] / r["packets"] if isinstance(r["packets"], int) and r["packets"] else "-"
        r["retx%"] = pct
        rows.append([run_label(path, files)] + [r[h] for h in header[1:]])
    print_table(header, rows, args.csv)


if __name__ == "__main__":
    main()
