"""Shared helpers for the log analysis scripts: find runs and print tables."""
import glob
import os
import re
import sys

import numpy as np


def results_root():
    """<ns-3>/results, found as commands/run.sh does ($NS3_DIR, parent, or ns-3.46 next to the repo)."""
    here = os.path.dirname(os.path.abspath(__file__))
    if os.environ.get("NS3_DIR"):
        ns3 = os.environ["NS3_DIR"]
    elif os.path.exists(os.path.join(here, "..", "ns3")):
        ns3 = os.path.join(here, "..")
    else:
        ns3 = os.path.join(here, "..", "..", "ns-3.46")
    return os.path.normpath(os.path.join(ns3, "results"))


def find_files(paths, pattern):
    """Files matching `pattern` (e.g. '*.fct') under every path: a file, a directory,
    or an experiment name under <ns-3>/results."""
    found = []
    for path in paths:
        if not os.path.exists(path) and os.path.exists(os.path.join(results_root(), path)):
            path = os.path.join(results_root(), path)
        if os.path.isfile(path):
            found.append(path)
        elif os.path.isdir(path):
            found.extend(glob.glob(os.path.join(path, "**", pattern), recursive=True))
        else:
            sys.exit(f"No such file or directory: {path}")
    if not found:
        sys.exit(f"No {pattern} file under {' '.join(paths)}")
    return sorted(set(found))


def run_label(path, all_paths):
    """Short name of the run a log belongs to: its run directory, relative to the common root."""
    run_dir = lambda p: os.path.dirname(os.path.dirname(os.path.abspath(p))) \
        if os.path.basename(os.path.dirname(p)) == "logs" else os.path.dirname(os.path.abspath(p))
    dirs = [run_dir(p) for p in all_paths]
    root = os.path.commonpath(dirs) if len(dirs) > 1 else os.path.dirname(dirs[0])
    label = os.path.relpath(run_dir(path), root)
    return label if label != "." else os.path.basename(run_dir(path))


def print_table(header, rows, csv_path=None):
    """Aligned table on stdout; also written as CSV when csv_path is given."""
    cells = [[str(h) for h in header]] + [[fmt(v) for v in row] for row in rows]
    widths = [max(len(r[i]) for r in cells) for i in range(len(header))]
    for i, r in enumerate(cells):
        print("  ".join(c.rjust(w) if j else c.ljust(w) for j, (c, w) in enumerate(zip(r, widths))))
        if i == 0:
            print("  ".join("-" * w for w in widths))
    if csv_path:
        with open(csv_path, "w") as f:
            for r in cells:
                f.write(",".join(r) + "\n")
        print(f"\nWritten {csv_path}")


def fmt(v):
    if isinstance(v, float):
        return f"{v:.3f}" if abs(v) < 1e6 else f"{v:.4g}"
    return str(v)


_RATE_UNITS = {"bps": 1, "kbps": 1e3, "mbps": 1e6, "gbps": 1e9, "tbps": 1e12}


def _rate(text):
    """'100Gbps' / '50000000000bps' -> bits per second."""
    m = re.match(r"([\d.]+)\s*([a-zA-Z]+)$", text.strip())
    return float(m.group(1)) * _RATE_UNITS[m.group(2).lower()]


def network_bound(log_path):
    """Bandwidth lower bound on the time to deliver one iteration's messages.

    Built from the run itself: the messages of the .fct log next to `log_path`
    (src, dst, size) and, in the run's out.txt, the topology and every degraded
    T0-T1 link. Every server must send and receive its bytes through its P NIC
    ports, every T0 must push its cross-T0 bytes up its T1 links and receive them
    down, and every T0 pair must fit through the T1s joining them (via T1 j at
    min(up, down) rate). The bound is the slowest of these, assuming perfect load
    balancing and no latency or header overhead:
      equal: each message split evenly over the planes, as the senders do;
      any:   free to shift load between planes (at least `equal`'s NIC part).
    Returns a dict of times in us per iteration (nic, up, down, pair, equal,
    any), or None when out.txt, the .fct log or the topology line is missing.
    """
    logs = os.path.dirname(os.path.abspath(log_path))
    stem = os.path.splitext(os.path.basename(log_path))[0]
    fct = os.path.join(logs, stem + ".fct")
    jct = os.path.join(logs, stem + ".jct")
    out = os.path.join(os.path.dirname(logs), "out.txt")
    if not all(os.path.exists(p) for p in (fct, jct, out)):
        return None
    text = open(out).read()
    m = re.search(r"Multi-plane topology: (\d+) planes x (\S+), (\d+) servers, (\d+) T0 \+ (\d+) T1", text)
    if not m:
        return None
    P, rate, N, A, B = int(m.group(1)), _rate(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
    spt = N // A
    cap = np.full((P, A, B), rate)
    for r, p, i, j in re.findall(r"Degraded link to (\S+): plane (\d+) T0 (\d+) - T1 (\d+)", text):
        cap[int(p), int(i), int(j)] = _rate(r)

    msgs = np.loadtxt(fct, delimiter=",", usecols=(2, 3, 4), dtype=np.int64, ndmin=2)
    iters = {line.split(".")[0] for line in open(jct).read().splitlines()[1:] if line.startswith("iter")}
    n_iter = max(1, len(iters))
    src, dst, size = msgs[:, 0], msgs[:, 1], msgs[:, 2].astype(float) * 8 / n_iter  # bits per iteration
    tx = np.bincount(src, size, minlength=N)
    rx = np.bincount(dst, size, minlength=N)
    t0s, t0d = src // spt, dst // spt
    cross = np.zeros((A, A))
    np.add.at(cross, (t0s, t0d), size)
    np.fill_diagonal(cross, 0)

    # cap[p, i, j]: link T0 i - T1 j of plane p, the same rate both ways
    up = cap.sum(axis=2)                                               # [p, i]: T0 i -> its T1s
    down = up                                                          # [p, k]: T1s -> T0 k
    pair = np.minimum(cap[:, :, None, :], cap[:, None, :, :]).sum(axis=3)  # [p, i, k] via every T1
    nic = max(tx.max(), rx.max()) / (P * rate)
    with np.errstate(divide="ignore", invalid="ignore"):
        eq_up = (cross.sum(axis=1)[None, :] / P / up).max()
        eq_down = (cross.sum(axis=0)[None, :] / P / down).max()
        eq_pair = np.nan_to_num(cross[None] / P / pair).max()
        any_up = (cross.sum(axis=1) / up.sum(axis=0)).max()
        any_down = (cross.sum(axis=0) / down.sum(axis=0)).max()
        any_pair = np.nan_to_num(cross / pair.sum(axis=0)).max()
    us = lambda s: s * 1e6
    return {"nic": us(nic), "up": us(eq_up), "down": us(eq_down), "pair": us(eq_pair),
            "equal": us(max(nic, eq_up, eq_down, eq_pair)),
            "any": us(max(nic, any_up, any_down, any_pair))}


def iteration_windows(log_path):
    """{iteration: (start_ns, end_ns)} from the .jct log next to `log_path`: iteration N
    runs from the end of iteration N-1 (the first phase start for N = 0) to its last
    phase end, its barrier, as in jct.py. None if there is no .jct log."""
    stem = os.path.splitext(os.path.abspath(log_path))[0]
    jct = stem + ".jct"
    if not os.path.exists(jct):
        return None
    ends, first = {}, None
    with open(jct) as f:
        next(f, None)  # header
        for line in f:
            name, start, end, *_ = line.strip().split(",")
            start, end = float(start), float(end)
            if start < 0:
                continue
            first = start if first is None else min(first, start)
            m = re.match(r"^iter(\d+)\.", name)
            if m:
                it = int(m.group(1))
                ends[it] = max(ends.get(it, end), end)
    windows, prev = {}, first
    for it in sorted(ends):
        windows[it] = (prev * 1e3, ends[it] * 1e3)
        prev = ends[it]
    return windows
