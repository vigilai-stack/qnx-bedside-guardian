#!/bin/sh
# Run on the QNX target, from /tmp/bedside-guardian.
# 1. unit + scenario tests  2. the two-bed demo  3. evidence for the video.
set -eu
cd "$(dirname "$0")/.."
echo "=== uname"
uname -a
echo "=== unit and scenario tests"
BG_SOURCE_DIR=. ./test_bg || { echo "tests failed"; exit 1; }
echo "=== demo (native message passing; service registered under /dev/name/local)"
BUILD=. BG_RT_PRIO=${BG_RT_PRIO:-30} sh scripts/demo.sh "${1:-50}" &
sleep 3
echo "=== the guardian as QNX sees it"
ls -l /dev/name/local/ 2>/dev/null || true
pidin -p bgd 2>/dev/null || true
wait
