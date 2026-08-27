#!/usr/bin/env bash
# Tier 3: throughput end-to-end against a Pulsar standalone broker.
#
#   init -> consume(bg) -> produce (start-time now+500ms, batches=20,
#                                   batch-size=50, concurrency=2)
#
# Asserts:
#   - producer CSV exact header `task_id,batch_id,completion_timestamp_ns`
#   - exactly concurrency*batches rows
#   - per-task batch_ids are 0..batches-1; per-task timestamps non-decreasing
#   - consumer summary reports received == 2000 for persistent (guaranteed
#     delivery), received > 0 for non-persistent
set -u

TRANSPORT="${1:?usage: throughput_integration.sh <persistent|non-persistent> <sync|async>}"
MODE="${2:?usage: throughput_integration.sh <persistent|non-persistent> <sync|async>}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../build}"
TPUT="$BUILD_DIR/throughput"

# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

WORKDIR="$(mktemp -d)"
cleanup() {
    stop_broker
    rm -rf "$WORKDIR"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

[ -x "$TPUT" ] || fail "throughput binary not found at $TPUT"

start_broker

CONCURRENCY=2
BATCHES=20
BATCH_SIZE=50
EXPECTED=$((CONCURRENCY * BATCHES * BATCH_SIZE))

ARGS=(--url "$PULSAR_URL" --admin-url "$PULSAR_ADMIN_URL"
      --transport "$TRANSPORT" -s 64 --topic rb-test-tput)

"$TPUT" init "${ARGS[@]}" || fail "init failed"
pass "init"

"$TPUT" consume "${ARGS[@]}" --id 0 --concurrency 2 --duration 15s \
    > "$WORKDIR/consume.out" 2>&1 &
CONSUME_PID=$!

# Let the consumer subscription establish before producing. For
# non-persistent topics messages published without a subscriber are dropped.
sleep 3

START=$("$TPUT" start-time 500ms) || fail "start-time failed"

"$TPUT" produce "${ARGS[@]}" --id 0 --concurrency "$CONCURRENCY" \
    --batches "$BATCHES" --batch-size "$BATCH_SIZE" \
    --start-time "$START" --send-mode "$MODE" \
    --output "$WORKDIR/tput.csv" > "$WORKDIR/produce.out" 2>&1 \
    || { cat "$WORKDIR/produce.out" >&2; fail "produce failed"; }

wait "$CONSUME_PID" || { cat "$WORKDIR/consume.out" >&2; fail "consume failed"; }

echo "--- produce.out ---"; cat "$WORKDIR/produce.out"
echo "--- consume.out ---"; cat "$WORKDIR/consume.out"

# CSV assertions
[ -f "$WORKDIR/tput.csv" ] || fail "producer CSV missing"
header="$(head -n 1 "$WORKDIR/tput.csv")"
[ "$header" = "task_id,batch_id,completion_timestamp_ns" ] \
    || fail "bad CSV header: $header"
pass "CSV header"

rows=$(($(wc -l < "$WORKDIR/tput.csv") - 1))
[ "$rows" -eq $((CONCURRENCY * BATCHES)) ] \
    || fail "expected $((CONCURRENCY * BATCHES)) rows, got $rows"
pass "CSV has exactly $rows rows"

awk -F, -v conc="$CONCURRENCY" -v batches="$BATCHES" 'NR > 1 {
    task = $1; batch = $2; ts = $3
    if (batch != seen[task]) {
        print "FAIL: task " task " expected batch_id " seen[task] ", got " batch
        bad = 1; exit 1
    }
    seen[task]++
    if (last_ts[task] != "" && ts < last_ts[task]) {
        print "FAIL: task " task " timestamp decreased at batch " batch
        bad = 1; exit 1
    }
    last_ts[task] = ts
} END {
    if (bad) exit 1
    for (t = 0; t < conc; t++) {
        if (seen[t] != batches) {
            print "FAIL: task " t " has " seen[t] " batches, expected " batches
            exit 1
        }
    }
}' "$WORKDIR/tput.csv" || fail "CSV batch-id/timestamp check failed"
pass "per-task batch ids 0..$((BATCHES - 1)), timestamps non-decreasing"

# stdout summaries (valkey format)
grep -Eq "Producer 0: sent $EXPECTED items in [0-9.]+s" "$WORKDIR/produce.out" \
    || fail "producer summary line missing"

received=$(sed -nE 's/^Consumer 0: received ([0-9]+) items in .*/\1/p' \
           "$WORKDIR/consume.out")
[ -n "$received" ] || fail "consumer summary line missing"
if [ "$TRANSPORT" = "persistent" ]; then
    [ "$received" -eq "$EXPECTED" ] \
        || fail "persistent: expected received == $EXPECTED, got $received"
    pass "consumer received all $EXPECTED items"
else
    [ "$received" -gt 0 ] || fail "non-persistent: expected received > 0"
    pass "consumer received $received items (> 0)"
fi

echo "throughput_integration ($TRANSPORT, $MODE): ALL PASS"
