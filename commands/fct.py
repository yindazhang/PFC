#!/usr/bin/env python3
"""Message (flow) completion times from the simulator's .fct logs.

Every line of a .fct log is one message, written once the sender has it all
acknowledged:  id,qp,src,dst,size,post_ns,receive_ns,ack_ns,fct_ns
(fct = receive - post: the time until the destination has every byte).

Usage:
  commands/fct.py RESULTS...             one row per run (a .fct file or any directory above it)
  commands/fct.py RESULTS --by-size      also split each run by message size
  commands/fct.py RESULTS --cdf out.csv  write the FCT CDF of every run
  commands/fct.py RESULTS --ack          use post -> ack (sender completion) instead
  commands/fct.py RESULTS --iter 1       only messages posted in the second iteration (0-based)

Columns: messages, mean / p50 / p90 / p99 / max FCT (us), and slowdown = FCT
divided by the time the message takes alone at --rate (serialization only,
without the base RTT).
"""
import argparse
import os
import sys

import numpy as np
import pandas as pd

from common import find_files, iteration_windows, print_table, run_label

COLUMNS = ["id", "qp", "src", "dst", "size", "post", "receive", "ack", "fct"]


def load(path, use_ack):
    df = pd.read_csv(path, header=None, names=COLUMNS)
    df["t"] = (df["ack"] - df["post"]) if use_ack else df["fct"]
    return df


def summarize(t_ns, size, rate_gbps):
    t = np.sort(t_ns.to_numpy()) / 1e3
    ideal = size.to_numpy() * 8 / rate_gbps / 1e3  # us
    slow = np.sort(t_ns.to_numpy() / 1e3 / np.maximum(ideal, 1e-9))
    q = lambda a, p: float(np.percentile(a, p)) if len(a) else float("nan")
    return [len(t), float(t.mean()), q(t, 50), q(t, 90), q(t, 99), float(t.max()),
            float(slow.mean()), q(slow, 99)]


def size_bin(size):
    for limit, label in [(64e3, "<64KB"), (1e6, "64KB-1MB"), (16e6, "1-16MB")]:
        if size < limit:
            return label
    return ">=16MB"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help=".fct files or directories containing them")
    ap.add_argument("--rate", type=float, default=400, help="per-server rate (Gbps) for slowdown, default 400")
    ap.add_argument("--ack", action="store_true", help="FCT until the sender's last ack instead of receive")
    ap.add_argument("--by-size", action="store_true", help="split every run by message size")
    ap.add_argument("--cdf", metavar="CSV", help="write run,fct_us,cdf for every run")
    ap.add_argument("--csv", metavar="CSV", help="also write the table as CSV")
    ap.add_argument("--iter", metavar="N[,N...]",
                    help="only messages posted in these iterations (0-based, e.g. 1 = the second; "
                         "boundaries from the run's .jct log)")
    args = ap.parse_args()
    select = sorted({int(x) for x in args.iter.split(",")}) if args.iter else None

    files = find_files(args.paths, "*.fct")
    header = ["run", "messages", "mean_us", "p50_us", "p90_us", "p99_us", "max_us", "slowdown", "p99_slowdown"]
    if args.by_size:
        header.insert(1, "size")
    rows = []
    cdf = []
    for path in files:
        label = run_label(path, files)
        df = load(path, args.ack)
        if select is not None:
            windows = iteration_windows(path)
            if windows is None:
                sys.exit(f"--iter needs the .jct log next to {path}")
            keep = np.zeros(len(df), dtype=bool)
            for it in select:
                if it in windows:
                    lo, hi = windows[it]
                    keep |= (df["post"] >= lo) & (df["post"] < hi)
            df = df[keep]
        if df.empty:
            rows.append([label] + (["-"] if args.by_size else []) + [0] + [float("nan")] * 7)
            continue
        if args.by_size:
            df["bin"] = df["size"].map(size_bin)
            for b, g in df.groupby("bin", sort=False):
                rows.append([label, b] + summarize(g["t"], g["size"], args.rate))
        else:
            rows.append([label] + summarize(df["t"], df["size"], args.rate))
        if args.cdf:
            t = np.sort(df["t"].to_numpy()) / 1e3
            idx = np.unique(np.linspace(0, len(t) - 1, min(len(t), 1000)).astype(int))
            cdf += [(label, t[i], (i + 1) / len(t)) for i in idx]
    print_table(header, rows, args.csv)
    if args.cdf:
        pd.DataFrame(cdf, columns=["run", "fct_us", "cdf"]).to_csv(args.cdf, index=False)
        print(f"Written {args.cdf}")


if __name__ == "__main__":
    main()
