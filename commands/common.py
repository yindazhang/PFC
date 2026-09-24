"""Shared helpers for the log analysis scripts: find runs and print tables."""
import glob
import os
import sys


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
