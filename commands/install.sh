#!/usr/bin/env bash

readonly WORK_DIR="/mydata"
readonly REPO_NAME="PFC"
readonly NS3_VERSION="3.46"
readonly SSH_EMAIL="1293516092@qq.com"

sudo apt-get update
sudo apt-get install -y cmake git openssh-client python3-pip python3-pandas python3-matplotlib wget

mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
sudo chmod -R 777 "$WORK_DIR"

if [[ -z "$(find "$HOME/.ssh" -maxdepth 1 -type f -name '*.pub' -print -quit)" ]]; then
    echo "No SSH public key found; generating an Ed25519 key."
    ssh-keygen -t ed25519 -C "$SSH_EMAIL" -f "$HOME/.ssh/id_ed25519" -N ""
    chmod 600 "$HOME/.ssh/id_ed25519"
    chmod 644 "$HOME/.ssh/id_ed25519.pub"
else
    echo "An SSH public key already exists; skipping key generation."
fi

cd "$WORK_DIR"

# Clone or update repo
if [ -d "$REPO_NAME" ]; then
    echo "Directory $REPO_NAME exists."
    echo "Starting git pull in $REPO_NAME..."
    # Use subshell to avoid changing current directory permanently
    (cd "$REPO_NAME" && git pull)
else
    echo "Directory $REPO_NAME does not exist."
    echo "Starting git clone..."
    if ! git clone "git@github.com:yindazhang/$REPO_NAME.git"; then
        echo "SSH clone failed, trying HTTPS..."
            git clone "https://github.com/yindazhang/$REPO_NAME.git"
    fi
fi

# Check and install ns3 if needed
NS_DIR="ns-$NS3_VERSION"
NS_TAR="ns-$NS3_VERSION.tar.bz2"
NS_URL="https://www.nsnam.org/releases/ns-$NS3_VERSION.tar.bz2"
if [ -d "$NS_DIR" ]; then
    echo "Directory $NS_DIR exists."
else
    echo "Directory $NS_DIR does not exist."
    echo "Downloading and installing ns-$NS3_VERSION..."
    if [ ! -f "$NS_TAR" ]; then
        wget -- "$NS_URL"
    else
        echo "$NS_TAR already downloaded."
    fi
    tar -xjf "$NS_TAR"
fi

# Replace src and scratch only when they exist in the repository.
NS3_DIR="$NS_DIR"
mkdir -p "$NS3_DIR/commands"
mkdir -p "$NS3_DIR/commands"
for directory in src scratch commands; do
    if [ -d "$REPO_NAME/$directory" ] && [ -d "$NS3_DIR" ]; then
        rm -rf "$NS3_DIR/$directory"
        cp -a "$REPO_NAME/$directory" "$NS3_DIR/"
    else
        echo "$REPO_NAME/$directory does not exist; skipping."
    fi
done

# Build ns3
cd "$NS3_DIR"
rm -rf build/optimized
./ns3 configure \
    --build-profile=optimized \
    --out=build/optimized \
    --disable-modules="lorawan;wifi;lte;nr;vr-app;wban" \
    --disable-tests \
    --disable-examples
./ns3 build
./test.py --no-build