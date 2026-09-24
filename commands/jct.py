#!/usr/bin/env python3
"""Job, iteration and collective completion times from the simulator's .jct logs.

Every line of a .jct log is one phase of the workload:
  phase,start_us,end_us,duration_us,tasks,done
with phases named iter<N>.<name>, e.g.
  iter0.allreduce.g3     collective of group 3 (allreduce / alltoall / permutation)
  iter0.allreduce.s1     data-parallel all-reduce of pipeline stage 1 (training)
  iter0.forward          every forward compute of the iteration (training)
  iter0.end              the global barrier that ends the iteration

Usage:
  commands/jct.py RESULTS...           per run: job time, iterations, collectives
  commands/jct.py RESULTS --phases     also the mean duration of every phase kind
  commands/jct.py RESULTS --iters      also every iteration of every run

Job time = last phase end - first phase start. Iteration N ends at its barrier
(iter<N>.*.end or iter<N>.end) and starts where the previous one ended.
Collective statistics are over every group / stage collective of every iteration.
"""
import argparse
import re
from collections import defaultdict

import numpy as np

from common import find_files, print_table, run_label

COLLECTIVE = re.compile(r"^iter\d+\.(.+)\.(g|s)\d+$")
PHASE = re.compile(r"^iter(\d+)\.(.+)$")


def load(path):
    phases = []
    with open(path) as f:
        next(f, None)  # header
        for line in f:
            name, start, end, dur, tasks, done = line.strip().split(",")
            phases.append((name, float(start), float(end), float(dur), int(tasks), int(done)))
    return phases


def analyze(phases):
    complete = all(done == tasks for _, _, _, _, tasks, done in phases)
    started = [p for p in phases if p[1] >= 0]
    job = max(p[2] for p in started) - min(p[1] for p in started) if started else float("nan")

    # Iterations end at their barrier (the latest phase end of the iteration)
    iter_end = defaultdict(float)
    for name, start, end, *_ in started:
        m = PHASE.match(name)
        if m:
            iter_end[int(m.group(1))] = max(iter_end[int(m.group(1))], end)
    iters = []
    prev = min(p[1] for p in started) if started else 0
    for it in sorted(iter_end):
        iters.append(iter_end[it] - prev)
        prev = iter_end[it]

    collectives = sorted(dur for name, _, _, dur, tasks, done in started
                         if COLLECTIVE.match(name) and done == tasks)
    kinds = defaultdict(list)
    for name, _, _, dur, *_ in started:
        m = PHASE.match(name)
        if m:
            kinds[re.sub(r"\.(g|s)\d+$", ".*", m.group(2))].append(dur)
    return complete, job, iters, collectives, kinds


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help=".jct files or directories containing them")
    ap.add_argument("--phases", action="store_true", help="mean duration of every phase kind")
    ap.add_argument("--iters", action="store_true", help="every iteration of every run")
    ap.add_argument("--csv", metavar="CSV", help="also write the main table as CSV")
    args = ap.parse_args()

    files = find_files(args.paths, "*.jct")
    rows, iter_rows, phase_rows = [], [], []
    q = lambda a, p: float(np.percentile(a, p)) if len(a) else float("nan")
    for path in files:
        label = run_label(path, files)
        complete, job, iters, coll, kinds = analyze(load(path))
        rows.append([label + ("" if complete else " (INCOMPLETE)"), job, len(iters),
                     float(np.mean(iters)) if iters else float("nan"),
                     len(coll), float(np.mean(coll)) if coll else float("nan"),
                     q(coll, 50), q(coll, 99), coll[0] if coll else float("nan"),
                     coll[-1] if coll else float("nan")])
        iter_rows += [[label, i, t] for i, t in enumerate(iters)]
        phase_rows += [[label, k, len(v), float(np.mean(v)), float(np.max(v))]
                       for k, v in sorted(kinds.items())]

    print_table(["run", "job_us", "iters", "iter_mean_us", "collectives", "coll_mean_us",
                 "coll_p50_us", "coll_p99_us", "coll_min_us", "coll_max_us"], rows, args.csv)
    if args.iters:
        print()
        print_table(["run", "iteration", "time_us"], iter_rows)
    if args.phases:
        print()
        print_table(["run", "phase", "count", "mean_us", "max_us"], phase_rows)


if __name__ == "__main__":
    main()
