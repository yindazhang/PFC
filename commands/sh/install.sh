#!/bin/bash
set -e  # Exit immediately if a command exits with a non-zero status
sudo chmod -R 777 /mydata/
sudo apt update
sudo apt-get install cmake python3-pip
pip3 install pandas matplotlib
# Change to working directory
cd /mydata

# Clone or update header-compress repo
WORKER_DIR="PFC"
if [ -d "$WORKER_DIR" ]; then
    echo "Directory $WORKER_DIR exists."
    echo "Starting git pull in $WORKER_DIR..."
    # Use subshell to avoid changing current directory permanently
    (cd "$WORKER_DIR" && git pull)
else
    echo "Directory $WORKER_DIR does not exist."
    echo "Starting git clone..."
    if ! git clone git@github.com:yindazhang/$WORKER_DIR.git; then
        echo "SSH clone failed, trying HTTPS..."
        git clone https://github.com/yindazhang/$WORKER_DIR.git
    fi
fi

# Check and install ns-allinone-3.46 if needed
NS_VERSION="3.46"
NS_DIR="ns-$NS_VERSION"
NS_TAR="$NS_DIR.tar.bz2"
NS_URL="https://www.nsnam.org/releases/$NS_TAR"
if [ -d "$NS_DIR" ]; then
    echo "Directory $NS_DIR exists."
else
    echo "Directory $NS_DIR does not exist."
    echo "Downloading and installing $NS_DIR..."
    if [ ! -f "$NS_TAR" ]; then
        wget "$NS_URL"
    else
        echo "$NS_TAR already downloaded."
    fi
    tar xjf "$NS_TAR"
fi

# Remove old src and scratch directories before copying new ones
rm -rf "$NS_DIR/src/" "$NS_DIR/scratch/"
# Copy src and scratch from header-compress to ns-3.46
cp -r "$WORKER_DIR/src/" "$NS_DIR/"
cp -r "$WORKER_DIR/scratch/" "$NS_DIR/"
# Build ns-3.46
cd "$NS_DIR/"
mkdir -p logs
./ns3 configure --build-profile=optimized --out=build/optimized
./ns3 build
# Kill any running ns3.46-header-compress processes
sudo killall -9 ns$NS_VERSION-$WORKER_DIR || echo "No running ns$NS_VERSION-$WORKER_DIR processes found."