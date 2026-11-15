#!/bin/sh
# Copy the QNX build, scenarios and demo script to a running QNX target.
#
#   ./qnx/deploy.sh TARGET_IP [USER]
#
# For the QEMU quick start image, get the IP with `mkqnximage --getip` from
# the image's folder (or run `ifconfig` on its serial console). Use the
# login documented for that image.
set -eu
IP=${1:?usage: deploy.sh TARGET_IP [USER]}
USER_NAME=${2:-root}
ARCH=${QNX_ARCH:-x86_64}
cd "$(dirname "$0")/.."
B="build-qnx-$ARCH"
[ -x "$B/bgd" ] || { echo "run qnx/build-qnx.sh first" >&2; exit 1; }
DEST=/tmp/bedside-guardian
ssh "$USER_NAME@$IP" "mkdir -p $DEST/scenarios/expected $DEST/scripts"
scp "$B/bgd" "$B/bgsim" "$B/bgack" "$B/bgverify" "$B/bgscn" "$B/test_bg" "$USER_NAME@$IP:$DEST/"
scp scenarios/*.scn scenarios/MANIFEST.sha256 "$USER_NAME@$IP:$DEST/scenarios/"
scp scenarios/expected/*.out "$USER_NAME@$IP:$DEST/scenarios/expected/"
scp scripts/demo.sh qnx/on-target.sh "$USER_NAME@$IP:$DEST/scripts/"
echo "Deployed to $USER_NAME@$IP:$DEST"
echo "On the target:  cd $DEST && sh scripts/on-target.sh"
