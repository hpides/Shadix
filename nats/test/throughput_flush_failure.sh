#!/usr/bin/env bash
# Failure-path test: an async produce run whose flush_batch() barrier fails
# must be fatal — nonzero exit, no CSV, no success summary — instead of
# silently producing a complete-looking result for items that were never
# delivered. Mirrors pulsar/test/throughput_flush_failure.sh.
#
# Scenario: the producer connects, then the broker is killed before the sends
# start. The buffered publishes land in the client's reconnect buffer, and
# the batch-boundary barrier (core: flush PING/PONG; js: wait for pending
# PubAcks, bounded by MaxWait) must surface the dead broker as a fatal error.
#
# Also checks that a producer that cannot connect at all exits cleanly with
# an error instead of calling std::terminate.
#
# Usage: throughput_flush_failure.sh <core|js> [TPUT_BIN]
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRANSPORT="${1:?usage: throughput_flush_failure.sh <core|js> [TPUT_BIN]}"
TPUT="${2:-${TPUT_BIN:-$SCRIPT_DIR/../build/throughput}}"

. "$SCRIPT_DIR/nats_fixture.sh"

pass() { echo "ok: $*"; }

[ -x "$TPUT" ] || fail "throughput binary not found/executable at $TPUT"

# --- unreachable broker: clean exit, not std::terminate ----------------------
"$TPUT" produce --url nats://127.0.0.1:1 --transport "$TRANSPORT" -s 64 \
    --id 0 --concurrency 1 --batches 1 --batch-size 1 \
    --start-time 1 --send-mode async \
    > "$WORKDIR/noconnect.out" 2>&1
rc=$?
[ "$rc" -eq 1 ] || fail \
    "produce against unreachable broker: expected clean exit 1, got $rc: $(cat "$WORKDIR/noconnect.out")"
grep -q "producer task 0 failed" "$WORKDIR/noconnect.out" \
    || fail "no task-failure diagnostic: $(cat "$WORKDIR/noconnect.out")"
pass "unreachable broker fails cleanly (exit 1)"

# --- broker dies before the sends start --------------------------------------
COMMON=(--url "$NATS_URL" --transport "$TRANSPORT" -s 64)
CSV="$WORKDIR/tput.csv"

"$TPUT" init "${COMMON[@]}" > "$WORKDIR/init.log" 2>&1 \
    || fail "init failed: $(cat "$WORKDIR/init.log")"
pass "init"

# Start far enough in the future that the producer connects and the broker
# can be killed before the first send happens.
START="$("$TPUT" start-time 5s)" || fail "start-time failed"

"$TPUT" produce "${COMMON[@]}" --id 0 --concurrency 1 \
    --batches 2 --batch-size 50 \
    --start-time "$START" --send-mode async \
    --output "$CSV" > "$WORKDIR/produce.out" 2>&1 &
PRODUCE_PID=$!

sleep 1  # local connect is fast; the producer is attached by now
echo "Killing nats-server (pid $SERVER_PID) mid-run"
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

# The buffered publishes sit in the reconnect buffer; the batch barrier must
# surface the dead broker as a fatal error (core: flush timeout ~5s; js:
# PubAck barrier MaxWait 30s).
if wait "$PRODUCE_PID"; then
    echo "--- produce.out ---"; cat "$WORKDIR/produce.out"
    fail "produce exited 0 despite a failed flush barrier"
fi
pass "produce exited nonzero"

echo "--- produce.out ---"; cat "$WORKDIR/produce.out"

[ ! -f "$CSV" ] || fail "producer CSV was written despite a failed run"
pass "no CSV written"

grep -Eq "Producer 0: sent" "$WORKDIR/produce.out" \
    && fail "success summary printed despite a failed run"
pass "no success summary"

grep -qi "fail" "$WORKDIR/produce.out" \
    || fail "no failure diagnostic in produce output"
pass "failure diagnostic present"

echo "throughput_flush_failure ($TRANSPORT): ALL PASS"
