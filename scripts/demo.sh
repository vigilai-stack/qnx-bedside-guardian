#!/bin/sh
# Runs the two-bed demo against a real bgd and writes demo-out/.
# Usage: sh scripts/demo.sh [SPEED_PCT]   (default 50 = twice as fast as the policy)
# On QNX, run it from the directory holding the binaries with BUILD=. .
set -eu
BUILD=${BUILD:-build}
SPEED=${1:-50}
OUT=${OUT:-demo-out}
SCN=${SCN:-scenarios/demo.scn}
mkdir -p "$OUT"
rm -f "$OUT/events.log" "$OUT/incidents.jsonl" "$OUT/bench-events.log"
export BG_SOCK_DIR=${BG_SOCK_DIR:-/tmp}

"$BUILD/bgd" --log "$OUT/events.log" --incidents "$OUT/incidents.jsonl" --scale "$SPEED" \
    --sync --rt "${BG_RT_PRIO:-0}" | tee "$OUT/bgd.out" &
sleep 1
echo "--- replaying $SCN at ${SPEED}% of real time"
"$BUILD/bgsim" --speed "$SPEED" "$SCN" | tee "$OUT/sim.out"
echo "--- console status"
"$BUILD/bgack" --status | tee "$OUT/status.out"
"$BUILD/bgack" --shutdown > /dev/null
wait

echo "--- 1000 round trips against a separate, quiet guardian instance"
"$BUILD/bgd" --name bg_bench --log "$OUT/bench-events.log" --incidents /dev/null --quiet \
    --rt "${BG_RT_PRIO:-0}" > "$OUT/bench-bgd.out" &
sleep 1
"$BUILD/bgsim" --name bg_bench --bench 1000 | tee "$OUT/bench.out"
"$BUILD/bgack" --name bg_bench --shutdown > /dev/null
wait
tail -1 "$OUT/bench-bgd.out"
echo "--- verifying the event log"
"$BUILD/bgverify" "$OUT/events.log" --html "$OUT/shift-report.html"
echo "--- incidents"
cat "$OUT/incidents.jsonl"
echo "Shift report: $OUT/shift-report.html"
