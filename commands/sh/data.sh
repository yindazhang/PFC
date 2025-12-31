#!/bin/bash
set -e  # Exit immediately if a command exits with a non-zero status
# Change to working directory
cd /mydata

WORKER_DIR="PFC"

NS_VERSION="3.46"
NS_DIR="ns-$NS_VERSION"

# Remove old commands directory and copy new commands from header-compress
rm -rf "$NS_DIR/commands/"
cp -r "$WORKER_DIR/commands/" "$NS_DIR/"
# Change to ns directory
cd "$NS_DIR/commands/"

# Define CDFs, loads, and durations for each CDF
CDFS=("Storage" "WebSearch" "Cache" "Hadoop")
LOADS=(0.3 0.4 0.5 0.6 0.7)
declare -A DURATION=( ["Storage"]=0.2 ["WebSearch"]=0.2 ["Cache"]=0.2 ["Hadoop"]=0.2 )

# Run traffic_gen.py for each CDF and load with corresponding duration
for cdf in "${CDFS[@]}"; do
    duration=${DURATION[$cdf]}
    for load in "${LOADS[@]}"; do
        python3 traffic_gen.py -l "$load" -c "$cdf" -t "$duration" &
    done
    wait  # Wait for all background jobs of this CDF to finish before continuing
    echo "Finished $cdf"
done
