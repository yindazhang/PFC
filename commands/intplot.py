#!/usr/bin/env python3
"""Heatmaps of the telemetry snapshot log (<run>.int): how loaded each monitored uplink is over time.

Usage:
  commands/intplot.py RESULTS...              every monitored switch and direction found
  commands/intplot.py RESULTS --leaf 0:0      only plane 0, T0 (ToR) 0
  commands/intplot.py RESULTS --agg 0:3       only plane 0, aggregation switch 3 (fat tree)
  commands/intplot.py RESULTS --dir out       only one direction (out | in)
  commands/intplot.py RESULTS --slot 20       20 us time slots (default: about 400 slots)
  commands/intplot.py RESULTS --out DIR       write the PNGs to DIR (default: next to the log)

RESULTS is a .int file, a directory, or an experiment name under <ns-3>/results.

A monitoring switch watches its uplinks in two directions:
  out  traffic it sends: its own egress queue towards each uplink, and bytes sent;
  in   traffic it receives: the upstream switch's egress queue towards it
       (carried by INT), and bytes received.
Uplinks: leaf-spine T0 -> T1 j; fat tree ToR -> aggregation switch j of the pod
(Agg j), aggregation switch -> core c of its group (Core c).

Each port has a queue level per direction: 0 below the first threshold, then
one more per threshold crossed (--intLevels, default 10 / 45 / 80 % of the port
buffer; back down 5 % below each). A snapshot is taken on every level change.

One figure per monitored switch and direction: x is time since the first
iteration started (us), y is the uplink, and each cell is one time slot:
  level    time-weighted mean queue level over the slot (0 .. number of
           thresholds), exact from the level changes;
  unused   capacity left on the link: plane rate - low-priority Gbps sent (out)
           or received (in), on a log scale (1 Gbps .. plane rate), so a link at
           90 Gbps stands out from one at 99; a degraded link shows at least half
           unused.
unused comes from the cumulative byte counters in each snapshot. Snapshots are
taken only on level changes, so the counters are interpolated linearly between
them: a slot shows the mean rate between the snapshots around it, finer where
the switch saw more changes.
Iteration boundaries (from the .jct log) are dashed; degraded links (from
out.txt) are marked in the y labels.

Patterns to look for: one row darker than the rest (imbalance), rows striped
on and off (oscillation), a row at a high level with low throughput (a degraded
link).

Also prints a table per uplink: the share of time at or above each threshold,
how often it rose past the first one, MB sent / received and its ratio to the
mean uplink. Older logs (busy/idle, with ECN columns) read as one threshold.
"""
import argparse
import csv
import os
import re
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LinearSegmentedColormap, LogNorm, Normalize

from common import _rate, find_files, iteration_windows, print_table

# Sequential ramps: zero recedes to the surface (white), magnitude darkens
BLUE = LinearSegmentedColormap.from_list(
    "level", ["#ffffff", "#cde2fb", "#86b6ef", "#3987e5", "#1c5cab", "#0d366b"])
AQUA = LinearSegmentedColormap.from_list(
    "unused", ["#ffffff", "#c4ecd9", "#7fd3ae", "#1baf7a", "#13805a", "#0b5039"])
UNUSED_FLOOR_GBPS = 1.0  # log scale: unused capacity below this shows as fully used


def read_int(path):
    """{(plane, kind, index, dir): [(time_ns, port, level, levels, bytes), ...]} in time order;
    kind is "T0" or "Agg", dir "out" or "in". Reads the three log formats: levels (since
    2026-10-26), busy/idle with both directions, and the first one (T0, received only)."""
    groups = defaultdict(list)
    with open(path) as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames or []
        for row in reader:
            if "t0" in fields:
                key = (int(row["plane"]), "T0", int(row["t0"]), "in")
                port, levels, nbytes = row["t1"], row["busy"], row["rx_bytes"]
            else:
                key = (int(row["plane"]), row["switch"], int(row["index"]), row["dir"])
                port = row["port"]
                levels = row["levels"] if "levels" in fields else row["busy"]
                nbytes = row["bytes"]
            level = int(row["level"]) if "level" in fields else int(row["state"] == "busy")
            groups[key].append((
                int(row["time_ns"]), int(port), level,
                np.array([int(c) for c in levels]),
                np.array([int(v) for v in nbytes.split(";")], dtype=float)))
    for rows in groups.values():
        rows.sort(key=lambda r: r[0])
    return groups


def run_info(path):
    """(plane rate in bps, {(plane, kind, index, uplink): degraded rate in bps}, fat-tree K or
    None, thresholds in % or None) from the run's out.txt."""
    logs = os.path.dirname(os.path.abspath(path))
    out = os.path.join(os.path.dirname(logs), "out.txt")
    if not os.path.exists(out):
        return None, {}, None, None
    text = open(out).read()
    m = re.search(r"(?:Multi-plane|Fat-tree) topology: \d+ planes x (\S+),", text)
    k = re.search(r"Fat-tree topology: .*?K=(\d+)", text)
    k = int(k.group(1)) if k else None
    degraded = {(int(p), "T0", int(i), int(j)): _rate(r) for r, p, i, j in
                re.findall(r"Degraded link to (\S+): plane (\d+) T0 (\d+) - T1 (\d+)", text)}
    if k:
        # Core c is uplink c - (g % K) * K of aggregation switch g
        for r, p, g, c in re.findall(r"Degraded link to (\S+): plane (\d+) Agg (\d+) - Core (\d+)", text):
            degraded[(int(p), "Agg", int(g), int(c) - int(g) % k * k)] = _rate(r)
    lv = re.search(r"Telemetry levels \(% of the port buffer\): up ([\d. ]+),", text)
    thresholds = [float(x) for x in lv.group(1).split()] if lv else None
    return (_rate(m.group(1)) if m else None), degraded, k, thresholds


def uplink_label(kind, index, j, k):
    """Name of uplink j of switch (kind, index)."""
    if kind == "Agg":
        return f"Core {index % k * k + j}" if k else f"Core {j}"
    return f"Agg {j}" if k else f"T1 {j}"


def switch_name(kind, index, k):
    return f"Agg {index}" if kind == "Agg" else (f"ToR {index}" if k else f"T0 {index}")


def nice_slot(span_ns, target=400):
    """A 1/2/5 x 10^k slot in ns giving about `target` slots over `span_ns`."""
    raw = max(span_ns / target, 1.0)
    k = 10 ** np.floor(np.log10(raw))
    return next(m * k for m in (1, 2, 5, 10) if m * k >= raw)


def analyze(rows, start_ns, slot_ns, n_levels):
    """Per uplink and slot: mean level and Gbps (sent or received); plus per-uplink totals."""
    times = np.array([r[0] for r in rows], dtype=float)
    n_up = len(rows[0][3])
    end_ns = times[-1]
    edges = np.arange(start_ns, end_ns + slot_ns, slot_ns)
    t = np.concatenate([[start_ns], times])
    at = lambda cum, j: np.interp(edges, t, cum[:, j])

    # Levels are piecewise constant between snapshots (all 0 before the first).
    # Time at or above each threshold L, cumulative: summed, the slot's mean level
    level = np.vstack([np.zeros(n_up), [r[3] for r in rows]])   # [snapshot, uplink], after it
    dt = np.diff(t)[:, None]
    cum_at = []
    for lv in range(1, n_levels + 1):
        cum_at.append(np.vstack([np.zeros(n_up), np.cumsum((level[:-1] >= lv) * dt, axis=0)]))
    mean_level = np.zeros((n_up, len(edges) - 1))
    for cum in cum_at:
        mean_level += np.array([np.diff(at(cum, j)) / slot_ns for j in range(n_up)])

    # Cumulative bytes: counters are 0 at the start, then sampled at every snapshot
    cum_bytes = np.vstack([np.zeros(n_up), [r[4] for r in rows]])
    gbps = np.array([np.diff(at(cum_bytes, j)) for j in range(n_up)]) * 8 / slot_ns

    # Per uplink: rises = changes from level 0 to above
    rises = np.zeros(n_up, dtype=int)
    prev = np.zeros(n_up)
    for r in rows:
        if prev[r[1]] == 0 and r[2] > 0:
            rises[r[1]] += 1
        prev = r[3]
    span = end_ns - start_ns
    total = cum_bytes[-1]
    mean_total = total.mean() if total.mean() > 0 else 1
    totals = []
    for j in range(n_up):
        totals.append({
            "at_pct": [100 * cum[-1, j] / span if span > 0 else 0.0 for cum in cum_at],
            "rises": int(rises[j]),
            "MB": total[j] / 1e6,
            "vs_mean": total[j] / mean_total,
        })
    return edges, mean_level, gbps, totals


def level_names(thresholds, n_levels):
    """Colorbar labels: the queue range of each level."""
    if not thresholds or len(thresholds) != n_levels:
        return [str(i) for i in range(n_levels + 1)]
    bounds = [f"{x:g}%" for x in thresholds]
    return [f"<{bounds[0]}"] + [f">{b}" for b in bounds]


def plot(path, key, edges, mean_level, gbps, start_ns, windows, plane_bps, degraded, out_png,
         k, n_levels, thresholds):
    plane, kind, index, direction = key
    n_up = mean_level.shape[0]
    x = (edges - start_ns) / 1e3
    y = np.arange(n_up + 1) - 0.5
    labels = []
    for j in range(n_up):
        rate = degraded.get((plane, kind, index, j))
        labels.append(uplink_label(kind, index, j, k) + (f"  ({rate / 1e9:g}G)" if rate else ""))
    flow = "sent" if direction == "out" else "received"

    fig, axes = plt.subplots(2, 1, figsize=(13, 1.6 + 2 * (0.6 + 0.22 * n_up)), sharex=True,
                             constrained_layout=True)
    rate_gbps = (plane_bps or np.nanmax(gbps) * 1e9) / 1e9
    unused = np.clip(rate_gbps - gbps, UNUSED_FLOOR_GBPS, rate_gbps)
    source = "own egress queue" if direction == "out" else "upstream queue, from INT"
    panels = [
        (mean_level, BLUE, Normalize(0, n_levels), f"queue level, mean over the slot ({source})"),
        (unused, AQUA, LogNorm(UNUSED_FLOOR_GBPS, rate_gbps),
         f"unused capacity: {rate_gbps:g} Gbps - low-priority Gbps {flow} (log scale)"),
    ]
    for ax, (data, cmap, norm, title) in zip(axes, panels):
        mesh = ax.pcolormesh(x, y, data, cmap=cmap, norm=norm, shading="flat")
        ax.set_yticks(range(n_up))
        ax.set_yticklabels(labels, fontsize=7)
        ax.invert_yaxis()
        ax.set_title(title, loc="left", fontsize=10)
        for spine in ax.spines.values():
            spine.set_visible(False)
        ax.tick_params(length=0)
        for d in degraded:
            if d[:3] == (plane, kind, index) and d[3] < n_up:
                ax.get_yticklabels()[d[3]].set_fontweight("bold")
        if windows:
            for it, (_, end) in windows.items():
                ax.axvline((end - start_ns) / 1e3, color="#6b6b66", lw=0.8, ls="--")
        bar = fig.colorbar(mesh, ax=ax, pad=0.01, fraction=0.025)
        if isinstance(norm, LogNorm):
            ticks = [t for t in (1, 2, 5, 10, 20, 50, 100, 200, 400) if t <= rate_gbps]
            bar.set_ticks(ticks)
            bar.set_ticklabels([f"{t:g}" for t in ticks])
        else:
            bar.set_ticks(range(n_levels + 1))
            bar.set_ticklabels(level_names(thresholds, n_levels))
    axes[-1].set_xlabel("time since the first iteration started (us)")
    slot_us = (edges[1] - edges[0]) / 1e3
    what = "traffic sent out" if direction == "out" else "traffic received (INT)"
    fig.suptitle(f"{os.path.basename(path)}\nplane {plane}, {switch_name(kind, index, k)}: uplinks, "
                 f"{what} (slot {slot_us:g} us)", fontsize=10, x=0.01, ha="left")
    fig.savefig(out_png, dpi=130)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help=".int files, directories, or experiment names")
    ap.add_argument("--leaf", action="append", metavar="P:I",
                    help="only plane P, T0 (ToR) I (repeatable)")
    ap.add_argument("--agg", action="append", metavar="P:I",
                    help="only plane P, aggregation switch I (fat tree; repeatable)")
    ap.add_argument("--dir", choices=("out", "in"), help="only this direction (default: both)")
    ap.add_argument("--slot", type=float, metavar="US", help="time slot in us (default: about 400 slots)")
    ap.add_argument("--out", metavar="DIR", help="directory for the PNGs (default: next to each log)")
    args = ap.parse_args()
    wanted = None
    if args.leaf or args.agg:
        wanted = {(int(p), "T0", int(i)) for p, i in (s.split(":") for s in args.leaf or [])}
        wanted |= {(int(p), "Agg", int(i)) for p, i in (s.split(":") for s in args.agg or [])}

    for path in find_files(args.paths, "*.int"):
        groups = read_int(path)
        if not groups:
            print(f"{path}: no snapshots")
            continue
        windows = iteration_windows(path)
        plane_bps, degraded, k, thresholds = run_info(path)
        n_levels = len(thresholds) if thresholds else max(
            int(r[3].max()) for rows in groups.values() for r in rows) or 1
        order = lambda key: (key[0], key[1] != "T0", key[2], key[3] != "out")
        for key in sorted(groups, key=order):
            plane, kind, index, direction = key
            if (wanted and key[:3] not in wanted) or (args.dir and direction != args.dir):
                continue
            rows = groups[key]
            start_ns = min(w[0] for w in windows.values()) if windows else rows[0][0]
            slot_ns = args.slot * 1e3 if args.slot else nice_slot(rows[-1][0] - start_ns)
            edges, mean_level, gbps, totals = analyze(rows, start_ns, slot_ns, n_levels)

            stem = os.path.splitext(os.path.basename(path))[0]
            out_dir = args.out or os.path.dirname(os.path.abspath(path))
            os.makedirs(out_dir, exist_ok=True)
            tag = ("t" if kind == "T0" else "agg") + str(index)
            out_png = os.path.join(out_dir, f"{stem}.p{plane}{tag}.{direction}.png")
            plot(path, key, edges, mean_level, gbps, start_ns, windows, plane_bps, degraded, out_png,
                 k, n_levels, thresholds)

            flow = "tx" if direction == "out" else "rx"
            print(f"\n{path}\nplane {plane}, {switch_name(kind, index, k)}, {direction}: {len(rows)} "
                  f"snapshots over {(rows[-1][0] - start_ns) / 1e3:.1f} us -> {out_png}")
            names = [f">{x:g}%" for x in thresholds] if thresholds else [f">=L{i}" for i in range(1, n_levels + 1)]
            header = ["uplink"] + [f"{n}_time%" for n in names] + ["rises", f"{flow}_MB", f"{flow}/mean"]
            table = []
            for j, tot in enumerate(totals):
                rate = degraded.get((plane, kind, index, j))
                table.append([uplink_label(kind, index, j, k) + (f" ({rate / 1e9:g}G)" if rate else "")]
                             + tot["at_pct"] + [tot["rises"], tot["MB"], tot["vs_mean"]])
            print_table(header, table)
            nbytes = np.array([t["MB"] for t in totals])
            first = np.array([t["at_pct"][0] for t in totals])
            print(f"{flow} max/mean {nbytes.max() / nbytes.mean() if nbytes.mean() else 0:.3f}, "
                  f"time above the first threshold % min {first.min():.1f} / mean {first.mean():.1f} "
                  f"/ max {first.max():.1f}")


if __name__ == "__main__":
    main()
