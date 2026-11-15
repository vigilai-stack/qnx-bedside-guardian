#!/bin/sh
# End-to-end: real daemon, real IPC, real clock (windows scaled 10x faster).
set -eu
BUILD=${BUILD:-build}
WORK=$(mktemp -d)
export BG_SOCK_DIR="$WORK"
NAME=bg_e2e_$$
cleanup() { [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

"$BUILD/bgd" --name "$NAME" --log "$WORK/events.log" --incidents "$WORK/incidents.jsonl" \
    --scale 10 --sync > "$WORK/bgd.out" 2>&1 &
PID=$!
i=0
while [ ! -S "$WORK/bg-$NAME.sock" ]; do
    i=$((i + 1)); [ $i -gt 50 ] && { echo "bgd did not start"; cat "$WORK/bgd.out"; exit 1; }
    sleep 0.1
done

"$BUILD/bgsim" --name "$NAME" --speed 10 scenarios/demo.scn > "$WORK/sim.out"
"$BUILD/bgack" --name "$NAME" --status > "$WORK/status.out"
"$BUILD/bgsim" --name "$NAME" --bench 200 > "$WORK/bench.out"
"$BUILD/bgack" --name "$NAME" --shutdown > /dev/null
wait "$PID"; PID=

fail() { echo "e2e FAIL: $1"; echo "--- bgd"; cat "$WORK/bgd.out"; echo "--- sim"; cat "$WORK/sim.out"; exit 1; }
grep -q "ERR_ROLE" "$WORK/sim.out"        || fail "under-tier ack was not refused"
grep -q "ERR_STALE" "$WORK/sim.out"       || fail "stale ack was not refused"
grep -q "OK_DUPLICATE" "$WORK/sim.out"    || fail "double press not counted once"
grep -q "INCIDENT" "$WORK/bgd.out"        || fail "no incident for the unanswered ventilator"
grep -q "ESCALATE.*CHARGE_RN" "$WORK/bgd.out" || fail "no escalation to charge nurse"
grep -q "open=0" "$WORK/status.out"       || fail "alarms left open: $(cat "$WORK/status.out")"
grep -q "failures=0" "$WORK/bench.out"    || fail "bench had failures"
grep -q '"event":"opened"' "$WORK/incidents.jsonl"       || fail "incident not written"
grep -q '"event":"acknowledged"' "$WORK/incidents.jsonl" || fail "incident ack not written"
grep -q 'Bedside nurse was in bed 6 with a fall.' "$WORK/incidents.jsonl" || fail "note not verbatim"
"$BUILD/bgverify" "$WORK/events.log" --html "$WORK/report.html" > "$WORK/verify.out" || fail "log chain broken"
grep -q "^OK:" "$WORK/verify.out"         || fail "verify output"
grep -q "Event log intact" "$WORK/report.html" || fail "report"

# Tamper with one character and make sure bgverify catches it.
sed 's/R. Okafor/R. 0kafor/' "$WORK/events.log" > "$WORK/tampered.log"
if "$BUILD/bgverify" "$WORK/tampered.log" > "$WORK/tamper.out"; then fail "tampering not detected"; fi
grep -q "BROKEN" "$WORK/tamper.out"       || fail "tamper output"

# A restarted daemon continues the same chain.
"$BUILD/bgd" --name "$NAME" --log "$WORK/events.log" --incidents "$WORK/incidents.jsonl" \
    --quiet > "$WORK/bgd2.out" 2>&1 &
PID=$!
i=0
while [ ! -S "$WORK/bg-$NAME.sock" ]; do i=$((i + 1)); [ $i -gt 50 ] && fail "restart"; sleep 0.1; done
"$BUILD/bgack" --name "$NAME" --shutdown > /dev/null
wait "$PID"; PID=
"$BUILD/bgverify" "$WORK/events.log" > /dev/null || fail "chain broken after restart"

echo "e2e ok: $(cat "$WORK/verify.out")"
cat "$WORK/bench.out"
