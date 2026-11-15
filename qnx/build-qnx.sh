#!/bin/sh
# Cross-compile QNX Bedside Guardian for a QNX 8.0 target with qcc.
#
#   ./qnx/build-qnx.sh                 # x86_64, native message passing
#   QNX_ARCH=aarch64le ./qnx/build-qnx.sh   # e.g. Raspberry Pi 5
#   TRANSPORT=posix ./qnx/build-qnx.sh      # socket transport, for comparison
#
# Needs QNX SDP 8.0 installed (free QNX Everywhere license). Output goes to
# build-qnx-<arch>/.
set -eu
SDP=${QNX_SDP:-$HOME/qnx800}
if [ -z "${QNX_HOST:-}" ]; then
    if [ -f "$SDP/qnxsdp-env.sh" ]; then
        # shellcheck disable=SC1091
        . "$SDP/qnxsdp-env.sh"
    else
        echo "QNX SDP not found. Install SDP 8.0 and set QNX_SDP, or source qnxsdp-env.sh." >&2
        exit 1
    fi
fi
command -v qcc >/dev/null || { echo "qcc not on PATH after sourcing the SDP" >&2; exit 1; }
cd "$(dirname "$0")/.."
ARCH=${QNX_ARCH:-x86_64}
make QNX=1 QNX_ARCH="$ARCH" TRANSPORT="${TRANSPORT:-native}" WERROR="${WERROR:-0}" \
    "build-qnx-$ARCH/bgd" "build-qnx-$ARCH/bgsim" "build-qnx-$ARCH/bgack" \
    "build-qnx-$ARCH/bgverify" "build-qnx-$ARCH/bgscn" "build-qnx-$ARCH/test_bg"
echo
echo "Built for QNX ($ARCH, transport ${TRANSPORT:-native}):"
ls -l "build-qnx-$ARCH"/bg* "build-qnx-$ARCH/test_bg"
file "build-qnx-$ARCH/bgd" 2>/dev/null || true
