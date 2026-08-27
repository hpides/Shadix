#!/usr/bin/env bash
# Tier 2: rtt end-to-end against a Pulsar standalone broker.
#
#   init -> server(bg) + receive(bg) -> send (2s, load 0.001, concurrency 2)
#
# The send mode (sync|async) is passed to both `send` and `server`; in async
# mode every request/echo goes through the client's batching producer and
# the per-thread flush() after the duration loop must settle them all.
#
# Asserts:
#   - receiver CSV exists with exact header `send_time_ns,receive_time_ns`
#   - > 0 data rows; every receive >= send; every RTT < 5e9 ns
#   - server/receiver stdout contain the valkey-format summary lines
set -u

TRANSPORT="${1:?usage: rtt_integration.sh <persistent|non-persistent> <sync|async>}"
MODE="${2:?usage: rtt_integration.sh <persistent|non-persistent> <sync|async>}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../build}"
RTT="$BUILD_DIR/rtt"

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

[ -x "$RTT" ] || fail "rtt binary not found at $RTT"

start_broker

ARGS=(--url "$PULSAR_URL" --admin-url "$PULSAR_ADMIN_URL"
      --transport "$TRANSPORT" -s 32
      --req-topic rb-test-rtt-req --resp-topic rb-test-rtt-resp)

"$RTT" "${ARGS[@]}" init || fail "init failed"
pass "init"

"$RTT" "${ARGS[@]}" server --id 0 --concurrency 2 --duration 10s \
    --send-mode "$MODE" > "$WORKDIR/server.out" 2>&1 &
SERVER_PID=$!
"$RTT" "${ARGS[@]}" receive --id 0 --concurrency 2 --duration 10s \
    --output "$WORKDIR/rtt.csv" > "$WORKDIR/receive.out" 2>&1 &
RECV_PID=$!

# Let the server/receiver subscriptions establish before sending. For
# non-persistent topics messages published without a subscriber are dropped.
sleep 3

"$RTT" "${ARGS[@]}" send --id 0 --concurrency 2 --duration 2s --load 0.001 \
    --send-mode "$MODE" > "$WORKDIR/send.out" 2>&1 \
    || { cat "$WORKDIR/send.out" >&2; fail "send failed"; }

wait "$SERVER_PID" || { cat "$WORKDIR/server.out" >&2; fail "server failed"; }
wait "$RECV_PID" || { cat "$WORKDIR/receive.out" >&2; fail "receive failed"; }

echo "--- send.out ---";    cat "$WORKDIR/send.out"
echo "--- server.out ---";  cat "$WORKDIR/server.out"
echo "--- receive.out ---"; cat "$WORKDIR/receive.out"

# CSV assertions
[ -f "$WORKDIR/rtt.csv" ] || fail "receiver CSV missing"
header="$(head -n 1 "$WORKDIR/rtt.csv")"
[ "$header" = "send_time_ns,receive_time_ns" ] \
    || fail "bad CSV header: $header"
pass "CSV header"

rows=$(($(wc -l < "$WORKDIR/rtt.csv") - 1))
[ "$rows" -gt 0 ] || fail "no data rows in receiver CSV"
pass "CSV has $rows data rows"

awk -F, 'NR > 1 {
    if ($2 < $1) { print "FAIL: receive < send on line " NR; bad = 1; exit 1 }
    if ($2 - $1 >= 5e9) { print "FAIL: RTT >= 5e9 ns on line " NR; bad = 1; exit 1 }
} END { exit bad }' "$WORKDIR/rtt.csv" || fail "CSV sanity check failed"
pass "every receive >= send and every RTT < 5e9 ns"

# stdout summaries (valkey format)
grep -Eq "Sender 0: sent [0-9]+ messages" "$WORKDIR/send.out" \
    || fail "sender summary line missing"
grep -Eq "Server 0: echoed [0-9]+ messages" "$WORKDIR/server.out" \
    || fail "server summary line missing"
grep -Eq "Receiver 0: received [0-9]+ messages" "$WORKDIR/receive.out" \
    || fail "receiver summary line missing"
grep -q "RTT Statistics" "$WORKDIR/receive.out" \
    || fail "RTT Statistics block missing"
pass "summary lines present"

echo "rtt_integration ($TRANSPORT, $MODE): ALL PASS"
