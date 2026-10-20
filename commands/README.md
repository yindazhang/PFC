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
  | `srv6all_trim` | MRC SRv6 source routing, all paths per plane active, trimming |
  | `reps_trim` | REPS over ECMP flow labels, trimming |
  | `srv6reps_trim` | REPS over SRv6 paths, trimming |

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
commands/run.sh -t rto --workload=alltoall --config=spray_lossy --ns3::RdmaSender::MinRto=200us,400us,1ms

# Topology sweep: oversubscribed (2:1), non-blocking, undersubscribed (1:2)
commands/run.sh -t sub -j 6 --workload=allreduce --config=spray_trim,srv6half_trim --t1=8,16,32

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
| `--ecnMark` | 1 | switches set the ECN bit (congestion control and path selection) |
| `--lastHopEcn` | 0 | also mark ECN on the last hop (T0 → server), which every path shares |
| `--lb` | mrc | load balancing: `mrc` (ECMP / spraying, or with `--srcRoute` the MRC active + backup EV sets) or `reps` (Recycled Entropy Packet Spraying, over flow labels or with `--srcRoute` over SRv6 paths; tune with `--ns3::RdmaSender::RepsBufferSize` (8), `RepsExplorePackets` (0 = one BDP per plane); no freezing mode) |
| `--ecnKmin` / `--ecnKmax` / `--ecnPmax` | 100 / 0 / 1.0 | RED marking of the ECN bit: thresholds in KB for a 100 Gbps port (scaled with the rate; Kmax 0 = 4 × Kmin) and the probability at Kmax |
| `--evOnEcn` / `--evOnLoss` | 1 / 1 | MRC SRv6 path switching on an ECN mark / on a trim NACK or an RTO |
| `--egressAdmission` | 1 | without PFC: shared-buffer admission per egress queue (0: per ingress port) |
| `--planes` / `--servers` | 4 / 256 | topology: planes (NICs per server) / servers (one GPU each) |
| `--t0` / `--t1` | 16 / 16 | topology: T0 / T1 switches per plane. `servers` must be a multiple of `t0` (at most 256). `t1` = servers per T0 is non-blocking; more is undersubscribed, fewer oversubscribed |

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
commands/fct.py perm-deg --iter 1           # only messages posted in the second iteration (0-based)
```

- **FCT** is post to receive: the time until the destination has every byte.
- **Slowdown** is the FCT divided by the message's time alone at `--rate`
  (default 400 Gbps, the 4 planes). It counts serialization only, not the RTT.

### Job, iteration and collective time: `commands/jct.py`

```bash
commands/jct.py ar                # job time, iteration time, collective mean/median/p99/min/max
commands/jct.py ar --iters        # every iteration
commands/jct.py ar --phases       # mean duration of every phase kind (forward, pipeline, ...)
commands/jct.py ar --bound        # which part limits the ideal (NIC, T0 up/down, T0 pair)
commands/jct.py ar --iter 1       # only the second iteration (0-based; e.g. skip the first while CC settles)
```

- **`ideal_us`** is a bandwidth lower bound on one iteration, computed from the
  run's own messages (`.fct`), topology and degraded links (`out.txt`). Every
  NIC (P × rate), every T0's uplinks and downlinks, and every T0 pair's T1
  paths (min of up and down per T1) must carry their bytes, with each message
  split evenly over the planes as the senders do. It ignores latency, headers
  and compute. `--bound` also shows `any_split_us`, the bound if load could
  shift between planes. **`eff`** = `ideal_us / iter_mean_us`.
  Example: a 32 MB ring all-reduce of 16 ranks has an ideal of 1.2 ms
  (2·15/16·32 MB at 400 Gbps); with 64 degraded links, 1.37 ms.

- **Collectives** are the per-group phases (`iterN.<type>.gK`) and, for
  training, the per-stage gradient all-reduces (`iterN.allreduce.sK`).
- **An iteration** ends at its global barrier.

### Transport and switch counters: `commands/stats.py`

```bash
commands/stats.py perm-deg
```

It shows data packets, retransmissions, RTOs, trim NACKs, EV swaps
(SRv6), and switch drops, trims, ECN marks and the largest egress queue. It also shows the T0→T1 load
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
