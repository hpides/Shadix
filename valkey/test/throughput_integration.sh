#!/usr/bin/env bash
# Tier 3: throughput end-to-end against a real Valkey server (docker).
#
# Usage: throughput_integration.sh <sync|async> [TPUT_BIN]
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:?usage: throughput_integration.sh <sync|async> [TPUT_BIN]}"
TPUT="${2:-${TPUT_BIN:-$SCRIPT_DIR/../build/throughput}}"

. "$SCRIPT_DIR/valkey_fixture.sh"

[ -x "$TPUT" ] || fail "throughput binary not found/executable at $TPUT"

CONCURRENCY=2
BATCHES=20
BATCH_SIZE=50
TOTAL=$((CONCURRENCY * BATCHES * BATCH_SIZE))

COMMON=(--url "$VALKEY_URL" --transport list -s 64)
CSV="$WORKDIR/tput.csv"

"$TPUT" init "${COMMON[@]}" > "$WORKDIR/init.log" 2>&1 \
    || fail "init failed: $(cat "$WORKDIR/init.log")"

"$TPUT" consume "${COMMON[@]}" --id 0 --concurrency 2 --duration 5s \
    > "$WORKDIR/consume.log" 2>&1 &
CONSUME_PID=$!

sleep 1  # let consumers connect (lists retain items either way)

START_TIME="$("$TPUT" start-time 500ms)" || fail "start-time failed"

"$TPUT" produce "${COMMON[@]}" --id 0 --concurrency "$CONCURRENCY" \
    --batches "$BATCHES" --batch-size "$BATCH_SIZE" \
    --start-time "$START_TIME" --send-mode "$MODE" --output "$CSV" \
    > "$WORKDIR/produce.log" 2>&1 \
    || { cat "$WORKDIR/produce.log" >&2; fail "produce failed"; }

wait "$CONSUME_PID" || { cat "$WORKDIR/consume.log" >&2; fail "consume failed"; }

# --- stdout summaries ---------------------------------------------------------
grep -q "Producer 0: sent $TOTAL items in" "$WORKDIR/produce.log" \
    || fail "producer summary missing: $(cat "$WORKDIR/produce.log")"
grep -q "Consumer 0: received" "$WORKDIR/consume.log" \
    || fail "consumer summary missing: $(cat "$WORKDIR/consume.log")"
echo "ok: stdout summaries"

# --- CSV ----------------------------------------------------------------------
[ -f "$CSV" ] || fail "producer CSV not written"
header="$(head -n1 "$CSV")"
[ "$header" = "task_id,batch_id,completion_timestamp_ns" ] \
    || fail "bad CSV header: $header"
rows=$(($(wc -l < "$CSV") - 1))
expected_rows=$((CONCURRENCY * BATCHES))
[ "$rows" -eq "$expected_rows" ] \
    || fail "expected $expected_rows CSV rows, got $rows"
echo "ok: CSV header + exactly $rows rows"

awk -F, -v batches="$BATCHES" -v conc="$CONCURRENCY" 'NR > 1 {
    t = $1; b = $2; ts = $3
    if (b != next_b[t]) {
        printf "task %s: expected batch_id %d, got %s\n", t, next_b[t], b
        exit 1
    }
    next_b[t]++
    if (seen[t] && ts < last_ts[t]) {
        printf "task %s: timestamp decreased at batch %s\n", t, b
        exit 1
    }
    seen[t] = 1; last_ts[t] = ts
}
END {
    ntasks = 0
    for (t in next_b) {
        ntasks++
        if (next_b[t] != batches) {
            printf "task %s: has %d batches, want %d\n", t, next_b[t], batches
            exit 1
        }
    }
    if (ntasks != conc) {
        printf "expected %d tasks, got %d\n", conc, ntasks
        exit 1
    }
}' "$CSV" || fail "CSV batch/timestamp check failed"
echo "ok: per-task batch_ids 0..$((BATCHES - 1)), timestamps non-decreasing"

# --- consumer counts ----------------------------------------------------------
received="$(grep -o 'received [0-9]*' "$WORKDIR/consume.log" | awk '{print $2}')"
[ -n "$received" ] || fail "could not parse consumer count: $(cat "$WORKDIR/consume.log")"
# Valkey lists are lossless: every item must arrive.
[ "$received" -eq "$TOTAL" ] \
    || fail "consumer received $received items, want exactly $TOTAL"
echo "ok: consumer received $received items"

echo "throughput_integration ($MODE): all checks passed"
