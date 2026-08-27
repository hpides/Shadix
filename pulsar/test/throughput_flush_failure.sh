#!/usr/bin/env bash
# Failure-path test: an async produce run whose flush() barrier fails must be
# fatal — nonzero exit, no CSV, no success summary — instead of silently
# producing a complete-looking result for items that were never delivered.
#
# Scenario: producers connect, then the broker is killed before the sends
# start. Every buffered sendAsync fails at the client send timeout, the
# batch-boundary flush() reports the failures, and `produce` must abort.
#
# This test kills its broker, so it always starts its own container on
# dedicated ports (26651/28081 by default) and never reuses an ambient one.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../build}"
TPUT="$BUILD_DIR/throughput"

# Dedicated ports: killing this broker must not disturb a reused one.
PULSAR_PORT="${PULSAR_FAILURE_PORT:-26651}"
PULSAR_ADMIN_PORT="${PULSAR_FAILURE_ADMIN_PORT:-28081}"
PULSAR_URL="pulsar://127.0.0.1:${PULSAR_PORT}"
PULSAR_ADMIN_URL="http://127.0.0.1:${PULSAR_ADMIN_PORT}"

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
[ -n "$BROKER_CONTAINER" ] || fail \
    "this test must own its broker; stop the one at ${PULSAR_ADMIN_URL}"

TOPIC=rb-test-flushfail
ARGS=(--url "$PULSAR_URL" --admin-url "$PULSAR_ADMIN_URL"
      --transport persistent -s 64 --topic "$TOPIC")

"$TPUT" init "${ARGS[@]}" || fail "init failed"
pass "init"

# Start far enough in the future that the producer connects and the broker
# can be killed before the first send happens.
START=$("$TPUT" start-time 10s) || fail "start-time failed"

"$TPUT" produce "${ARGS[@]}" --id 0 --concurrency 1 \
    --batches 2 --batch-size 50 \
    --start-time "$START" --send-mode async \
    --output "$WORKDIR/tput.csv" > "$WORKDIR/produce.out" 2>&1 &
PRODUCE_PID=$!

# Wait until the producer has attached to the topic, then kill the broker.
STATS_URL="${PULSAR_ADMIN_URL}/admin/v2/persistent/public/default/${TOPIC}/stats"
for _ in $(seq 1 16); do
    if curl -sf -m 3 "$STATS_URL" 2>/dev/null | grep -q '"producerName"'; then
        break
    fi
    sleep 0.5
done
echo "Killing broker container ${BROKER_CONTAINER} mid-run"
docker rm -f "$BROKER_CONTAINER" >/dev/null 2>&1 || true
BROKER_CONTAINER=""

# The buffered sends fail at the client send timeout (30s default); the
# flush() barrier must surface that as a fatal error.
if wait "$PRODUCE_PID"; then
    echo "--- produce.out ---"; cat "$WORKDIR/produce.out"
    fail "produce exited 0 despite a failed flush() barrier"
fi
pass "produce exited nonzero"

echo "--- produce.out ---"; cat "$WORKDIR/produce.out"

[ ! -f "$WORKDIR/tput.csv" ] \
    || fail "producer CSV was written despite a failed run"
pass "no CSV written"

grep -Eq "Producer 0: sent" "$WORKDIR/produce.out" \
    && fail "success summary printed despite a failed run"
pass "no success summary"

grep -qi "fail" "$WORKDIR/produce.out" \
    || fail "no failure diagnostic in produce output"
pass "failure diagnostic present"

echo "throughput_flush_failure: ALL PASS"
