# Running experiments and reading the results

The examples are run from the repository root (`/mydata/PFC`). The scripts find
the ns-3 tree themselves: `$NS3_DIR` if set, otherwise `ns-3.46` next to the
repository (`/mydata/ns-3.46`). Results go to `<ns-3>/results/`.
Project history, design decisions and open problems: [NOTES.md](NOTES.md).

## 1. Run: `commands/run.sh`

```bash
commands/run.sh [script options] [simulator options]
```

`run.sh` builds ns-3, then runs the simulator (`scratch/pfc.cc`) once for
every combination of the options you give it.

- **Simulator options** (`--key=value`) are passed through unchanged.
  `commands/run.sh --help-sim` lists all of them.
- **A comma-separated value is a sweep.** For example,
  `--degradeLinks=0,64,256 --config=ecmp_pfc,spray_trim` is 6 runs.
- **`--config=NAME` is a network preset:**

  | preset | meaning |
  |---|---|
  | `ecmp_pfc` | per-flow ECMP, PFC (lossless) |
  | `spray_pfc` | packet spraying, PFC |
  | `spray_lossy` | packet spraying, drop on overflow |
  | `spray_trim` | packet spraying, packet trimming |
  | `srv6half_lossy` | MRC SRv6 source routing, half the paths active, drop |
  | `srv6half_trim` | MRC SRv6 source routing, half the paths active, trimming |
  | `srv6all_trim` | MRC SRv6 source routing, all 16 paths/plane active, trimming |

- **Script options** go before the simulator options:

  | option | meaning |
  |---|---|
  | `-t NAME` | experiment name; results go to `results/NAME/` (default: date-time) |
  | `-j N` | runs in parallel (default 4; each uses about 1 core and 1 GB) |
  | `-n` | dry run: print the commands only |
  | `-d` | detach: keep running after you log out (log in `results/NAME/run.log`) |
  | `-B` | skip the build (needed while other simulations are running) |
  | `-o DIR` | results root other than `results/` |

### Examples

```bash
# One run with the defaults: 16 all-reduces of 16 ranks, 256 MB per rank, spray + trim
commands/run.sh -t ar --workload=allreduce --config=spray_trim

# Load balancing under link degradation: 3 configs x 3 fabrics = 9 runs, 6 at a time, detached
commands/run.sh -t perm-deg -j 6 -d --workload=permutation \
    --config=ecmp_pfc,spray_trim,srv6half_trim --degradeLinks=0,64,256

# Training with 4 or 8 pipeline stages, 2 iterations
commands/run.sh -t pp --workload=training --iters=2 --pp=4,8 --config=spray_trim

# Tune a model attribute directly (any ns-3 attribute works)
commands/run.sh -t rto --workload=alltoall --config=spray_lossy --ns3::RdmaSender::MinRto=100us,200us

# Check what would run before starting a big sweep
commands/run.sh -n --workload=allreduce --config=ecmp_pfc,spray_trim --bytes=16000000,256000000
```

### Main workload options

| option | default | meaning |
|---|---|---|
| `--workload` | `training` | `training`, `allreduce`, `alltoall`, `permutation` |
| `--iters` | 2 | iterations, separated by a global barrier |
| `--bytes` | 256000000 | allreduce / alltoall / permutation: bytes per rank |
| `--groupSize` | 16 | ranks per collective; all groups run at the same time (0 = one collective over all ranks) |
| `--placement` | `strided` | rank to server: `strided` (consecutive ranks on different T0s), `packed`, `random` |
| `--channels` / `--sliceBytes` / `--slots` | 2 / 512 KB / 8 | ring all-reduce pipelining |
| `--pp` / `--mb` | 4 / 4 | training: pipeline stages / micro-batches |
| `--act` / `--grad` | 8 MB / 64 MB | training: activation per micro-batch / gradient per rank |
| `--fwdUs` / `--bwdUs` | 500 / 1000 | training: compute per micro-batch (µs) |
| `--degradeLinks` / `--degradeRate` | 0 / 50Gbps | random T0-T1 links slowed down |

Large runs take a while. At the default sizes, a run takes roughly 15–60 minutes
and about 1 GB. Use `--bytes=16000000` (or smaller `--grad`/`--act`) for quick
tests.

### What a run leaves behind

```
results/NAME/
  progress.txt              "done <run> rc=0" per finished run, ALLDONE at the end
  <run>/                    e.g. spray_trim__degradeLinks-64
    cmd.sh                  the exact command; `bash cmd.sh` reruns just this run
    out.txt                 simulator output: counters, fabric load, phase table, collectives summary
    time.txt                wall-clock time and peak memory
    logs/<name>.fct         one line per message: id,qp,src,dst,size,post_ns,receive_ns,ack_ns,fct_ns
    logs/<name>.jct         one line per phase:   phase,start_us,end_us,duration_us,tasks,done
```

To follow a detached experiment: `tail -f results/NAME/progress.txt`.

## 2. Analyse

Each script takes an experiment name (looked up in `<ns-3>/results/`), any
directory (every run inside it becomes one row), or log files directly.
`--csv FILE` also saves the table.

### Message completion time: `commands/fct.py`

```bash
commands/fct.py perm-deg                    # per run: mean/p50/p90/p99/max FCT, slowdown
commands/fct.py perm-deg --by-size          # split by message size
commands/fct.py perm-deg --cdf cdf.csv      # CDF points (run,fct_us,cdf) for plotting
commands/fct.py perm-deg --ack              # until the sender's last ack instead of receive
```

- **FCT** is post to receive: the time until the destination has every byte.
- **Slowdown** is the FCT divided by the message's time alone at `--rate`
  (default 400 Gbps, the 4 planes). It counts serialization only, not the RTT.

### Job, iteration and collective time: `commands/jct.py`

```bash
commands/jct.py ar                # job time, iteration time, collective mean/median/p99/min/max
commands/jct.py ar --iters        # every iteration
commands/jct.py ar --phases       # mean duration of every phase kind (forward, pipeline, ...)
```

- **Collectives** are the per-group phases (`iterN.<type>.gK`) and, for
  training, the per-stage gradient all-reduces (`iterN.allreduce.sK`).
- **An iteration** ends at its global barrier.

### Transport and switch counters: `commands/stats.py`

```bash
commands/stats.py perm-deg
```

It shows data packets, retransmissions, RTOs, trim NACKs, EVs retired and restored
(SRv6), and switch drops, trims and ECN marks. It also shows the T0→T1 load
imbalance (max/mean bytes per link), wall-clock time and memory. Status is `ok`,
`running/failed`, or the number of tasks done when the run hit the time limit.

## Notes

- **Don't rebuild while runs are going.** Runs that start afterwards would use
  the new binary. `run.sh` refuses to build while simulations are running;
  either wait, or pass `-B` to reuse the current build.
- **Results are deterministic.** The same command gives the same numbers, and
  every configuration makes the same random choices, so comparisons are fair.
- **`<ns-3>/results/smoke/`** is a small example sweep (1 MB permutation, 4 runs) you
  can try the scripts on. Delete it whenever you like.
