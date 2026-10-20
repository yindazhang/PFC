#!/bin/bash
# Build and run the multi-plane MRC simulator (scratch/pfc.cc), one run or a sweep.
#
#   commands/run.sh [script options] [simulator options]
#
# Simulator options are passed to pfc as they are (see `commands/run.sh --help-sim`).
# A comma-separated value is a sweep: every combination of the swept options is
# one run. --config=NAME[,NAME...] expands to a preset of network options:
#
#   ecmp_pfc        per-flow ECMP, PFC (lossless)
#   spray_pfc       packet spraying, PFC
#   spray_lossy     packet spraying, drop on overflow
#   spray_trim      packet spraying, trimming
#   srv6half_lossy  MRC SRv6 source routing, half of the paths active, drop
#   srv6half_trim   MRC SRv6 source routing, half of the paths active, trimming
#   srv6all_trim    MRC SRv6 source routing, all paths per plane active, trimming
#   reps_trim       REPS over ECMP flow labels, trimming
#   srv6reps_trim   REPS over SRv6 paths, trimming
#
# Topology (defaults 4 planes, 256 servers, 16 T0 + 16 T1 per plane, non-blocking):
#   --planes=P --servers=N --t0=A --t1=B    N servers / A T0s per T0; B > N/A is
#   undersubscribed, B < N/A oversubscribed. Like any option they can be swept.
#
# Congestion marks:
#   --ecnMark=0|1   ECN bit, read by congestion control and path selection, on
#                   every port or none (default on)
#   --lastHopEcn=0|1  also mark ECN on the last hop T0 -> server (default 0)
#   --ecnKmin/--ecnKmax/--ecnPmax  RED for the ECN bit (default 100/400 KB, 1.0)
#   --evOnEcn=0|1   SRv6 path switching on an ECN mark (default 1)
#   --evOnLoss=0|1  SRv6 path switching on a trim NACK or an RTO (default 1)
#
# Load balancing:
#   --lb=mrc|reps   mrc (default): ECMP / spraying, or with --srcRoute the MRC
#                   active + backup EV sets; reps: Recycled Entropy Packet
#                   Spraying, over flow labels or (--srcRoute) SRv6 paths. Tune with
#                   --ns3::RdmaSender::RepsBufferSize / RepsExplorePackets
#
# Script options:
#   -t NAME   experiment name: results go to <results>/NAME (default: date-time)
#   -o DIR    results root (default: <ns-3>/results)
#
# The ns-3 tree is $NS3_DIR, or the parent of commands/ when that is ns-3, or
# ns-3.46 next to the repository (/mydata/PFC/commands -> /mydata/ns-3.46).
#   -j N      runs in parallel (default: 4; each takes one core and ~1 GB)
#   -n        dry run: create nothing, print the commands
#   -d        detach: keep running after the shell exits (log: <experiment>/run.log)
#   -B        do not build first
#
# Every run gets its own directory <results>/NAME/<swept values>/ (e.g.
# spray_trim__degradeLinks-64) with
#   cmd.sh    the exact command (rerun a single run with `bash cmd.sh`)
#   out.txt   simulator output (stats, phases, collectives summary)
#   time.txt  wall-clock seconds and peak memory
#   logs/     <name>.fct (one line per message) and <name>.jct (one line per phase)
# and <results>/NAME/progress.txt gets "done <run> rc=<exit code>" as runs finish.
#
# Examples:
#   commands/run.sh --workload=allreduce --config=spray_trim
#   commands/run.sh -t deg -j 6 --workload=permutation --config=ecmp_pfc,spray_trim,srv6half_trim \
#                   --degradeLinks=0,64,256
#   commands/run.sh -n --workload=training --iters=2 --pp=4,8
#   commands/run.sh -t sub -j 6 --workload=allreduce --config=spray_trim,srv6half_trim --t1=8,16,32

set -euo pipefail

# ns-3 tree: $NS3_DIR, else the parent of commands/ if it is ns-3, else ns-3.46 next to the repo
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -n "${NS3_DIR:-}" ]]; then
    ROOT="$(cd "$NS3_DIR" && pwd)"
elif [[ -x "$HERE/../ns3" ]]; then
    ROOT="$(cd "$HERE/.." && pwd)"
else
    ROOT="$(cd "$HERE/../.." && pwd)/ns-3.46"
fi
[[ -x "$ROOT/ns3" ]] || { echo "No ns-3 tree at $ROOT: set NS3_DIR=/path/to/ns-3.46" >&2; exit 1; }
BIN="$ROOT/build/optimized/scratch/ns3.46-pfc-optimized"
LIB="$ROOT/build/optimized/lib"

name="$(date +%Y%m%d-%H%M%S)"
results="$ROOT/results"
jobs=4
dryRun=0
detach=0
build=1

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi
if [[ "${1:-}" == "--help-sim" ]]; then
    LD_LIBRARY_PATH="$LIB" "$BIN" --help
    exit 0
fi

# Script options (single dash) come first; everything from the first --option on is the simulator's
while [[ $# -gt 0 && "$1" == -* && "$1" != --* ]]; do
    case "$1" in
    -t) name="$2"; shift ;;
    -o) results="$2"; shift ;;
    -j) jobs="$2"; shift ;;
    -n) dryRun=1 ;;
    -d) detach=1 ;;
    -B) build=0 ;;
    *)
        echo "Unknown script option $1 (see commands/run.sh -h)" >&2
        exit 1
        ;;
    esac
    shift
done

preset() {
    case "$1" in
    ecmp_pfc) echo "--spray=0 --srcRoute=0 --pfc=1 --trim=0" ;;
    spray_pfc) echo "--spray=1 --srcRoute=0 --pfc=1 --trim=0" ;;
    spray_lossy) echo "--spray=1 --srcRoute=0 --pfc=0 --trim=0" ;;
    spray_trim) echo "--spray=1 --srcRoute=0 --pfc=0 --trim=1" ;;
    srv6half_lossy) echo "--srcRoute=1 --pfc=0 --trim=0" ;;
    srv6half_trim) echo "--srcRoute=1 --pfc=0 --trim=1" ;;
    srv6all_trim) echo "--srcRoute=1 --pfc=0 --trim=1 --ns3::RdmaSender::EvsPerPlane=65535" ;; # clamped to the paths per plane
    reps_trim) echo "--lb=reps --spray=1 --srcRoute=0 --pfc=0 --trim=1" ;;
    srv6reps_trim) echo "--lb=reps --srcRoute=1 --pfc=0 --trim=1" ;;
    *)
        echo "Unknown --config preset '$1'" >&2
        exit 1
        ;;
    esac
}

# Split the options into fixed ones and swept ones (comma-separated values)
fixed=()
sweepKeys=()
sweepValues=()
for arg in "$@"; do
    if [[ "$arg" != --*=* ]]; then
        echo "Expected --option=value, got '$arg'" >&2
        exit 1
    fi
    key="${arg%%=*}"
    key="${key#--}"
    value="${arg#*=}"
    if [[ "$value" == *,* ]]; then
        sweepKeys+=("$key")
        sweepValues+=("$value")
    elif [[ "$key" == "config" ]]; then
        # A single preset is still recorded in the run name
        sweepKeys+=("$key")
        sweepValues+=("$value")
    else
        fixed+=("$arg")
    fi
done

# Cartesian product of the swept values: one "k1=v1 k2=v2 ..." line per run
combos=("")
for i in "${!sweepKeys[@]}"; do
    next=()
    IFS=',' read -ra values <<<"${sweepValues[$i]}"
    for combo in "${combos[@]}"; do
        for v in "${values[@]}"; do
            next+=("${combo:+$combo }${sweepKeys[$i]}=$v")
        done
    done
    combos=("${next[@]}")
done

expDir="$results/$name"
if [[ $dryRun -eq 0 ]]; then
    if [[ -e "$expDir" ]]; then
        echo "$expDir already exists: pick another -t NAME" >&2
        exit 1
    fi
    if [[ $build -eq 1 ]]; then
        if pgrep -x "$(basename "$BIN" | cut -c1-15)" >/dev/null; then
            echo "Simulations are running: building would change their binary. Use -B or wait." >&2
            exit 1
        fi
        echo "Building ..."
        (cd "$ROOT" && ./ns3 build >/dev/null) || { echo "Build failed (run ./ns3 build to see why)" >&2; exit 1; }
    fi
    mkdir -p "$expDir"
fi

for combo in "${combos[@]}"; do
    args=("${fixed[@]}")
    runName=""
    for kv in $combo; do
        key="${kv%%=*}"
        value="${kv#*=}"
        if [[ "$key" == "config" ]]; then
            runName+="${runName:+__}$value"
            read -ra expanded <<<"$(preset "$value")"
            args+=("${expanded[@]}")
        else
            runName+="${runName:+__}${key##*::}-$value"
            args+=("--$key=$value")
        fi
    done
    runName="${runName:-run}"
    runDir="$expDir/$runName"
    cmd="LD_LIBRARY_PATH=$LIB $BIN ${args[*]}"
    if [[ $dryRun -eq 1 ]]; then
        echo "[$runName] $cmd"
        continue
    fi
    mkdir -p "$runDir"
    cat >"$runDir/cmd.sh" <<EOF
#!/bin/bash
# Run: $runName
cd "\$(dirname "\${BASH_SOURCE[0]}")"
LD_LIBRARY_PATH=$LIB /usr/bin/time -f "%e s %M KB" -o time.txt \\
    $BIN ${args[*]} > out.txt 2>&1
rc=\$?
echo "done $runName rc=\$rc" >> ../progress.txt
exit \$rc
EOF
    chmod +x "$runDir/cmd.sh"
done

[[ $dryRun -eq 1 ]] && exit 0

echo "${#combos[@]} run(s) in $expDir, $jobs at a time"
runAll() {
    find "$expDir" -mindepth 2 -maxdepth 2 -name cmd.sh | sort | xargs -P "$jobs" -I{} bash {} || true
    echo ALLDONE >>"$expDir/progress.txt"
}
if [[ $detach -eq 1 ]]; then
    export -f runAll
    export expDir jobs
    setsid nohup bash -c runAll >"$expDir/run.log" 2>&1 </dev/null &
    echo "Detached. Progress: $expDir/progress.txt (ALLDONE at the end)"
else
    runAll
    echo "Finished: $(grep -c 'rc=0' "$expDir/progress.txt" || true)/${#combos[@]} succeeded"
fi
