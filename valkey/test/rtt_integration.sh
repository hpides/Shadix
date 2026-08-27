#!/usr/bin/env bash
# Tier 2: rtt end-to-end against a real Valkey server (docker).
#
# Usage: rtt_integration.sh <sync|async> [RTT_BIN]
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:?usage: rtt_integration.sh <sync|async> [RTT_BIN]}"
RTT="${2:-${RTT_BIN:-$SCRIPT_DIR/../build/rtt}}"

. "$SCRIPT_DIR/valkey_fixture.sh"

[ -x "$RTT" ] || fail "rtt binary not found/executable at $RTT"

COMMON=(--url "$VALKEY_URL" --transport list -s 64)
CSV="$WORKDIR/rtt.csv"

# init: reset broker state (DEL both keys).
"$RTT" "${COMMON[@]}" init > "$WORKDIR/init.log" 2>&1 \
    || fail "init failed: $(cat "$WORKDIR/init.log")"

# server + receiver in the background, then a 2s send burst.
"$RTT" "${COMMON[@]}" server --id 0 --concurrency 2 --duration 6s \
    --send-mode "$MODE" > "$WORKDIR/server.log" 2>&1 &
SERVER_ROLE_PID=$!
"$RTT" "${COMMON[@]}" receive --id 0 --concurrency 2 --duration 6s \
    --output "$CSV" > "$WORKDIR/receive.log" 2>&1 &
RECEIVE_PID=$!

sleep 1  # let server/receiver connect so early samples are not skewed by queueing

"$RTT" "${COMMON[@]}" send --id 0 --concurrency 2 --duration 2s --load 0.001 \
    --send-mode "$MODE" > "$WORKDIR/send.log" 2>&1 \
    || { cat "$WORKDIR/send.log" >&2; fail "send failed"; }

wait "$SERVER_ROLE_PID" || { cat "$WORKDIR/server.log" >&2; fail "server role failed"; }
wait "$RECEIVE_PID" || { cat "$WORKDIR/receive.log" >&2; fail "receive role failed"; }

# --- stdout summaries ---------------------------------------------------------
grep -q "Sender 0: sent" "$WORKDIR/send.log" \
    || fail "send summary missing: $(cat "$WORKDIR/send.log")"
grep -q "Server 0: echoed" "$WORKDIR/server.log" \
    || fail "server summary missing: $(cat "$WORKDIR/server.log")"
grep -q "Receiver 0: received" "$WORKDIR/receive.log" \
    || fail "receiver summary missing: $(cat "$WORKDIR/receive.log")"
grep -q "RTT Statistics" "$WORKDIR/receive.log" \
    || fail "RTT Statistics block missing: $(cat "$WORKDIR/receive.log")"
echo "ok: stdout summaries"

# --- CSV ----------------------------------------------------------------------
[ -f "$CSV" ] || fail "receiver CSV not written"
header="$(head -n1 "$CSV")"
[ "$header" = "send_time_ns,receive_time_ns" ] \
    || fail "bad CSV header: $header"
rows=$(($(wc -l < "$CSV") - 1))
[ "$rows" -gt 0 ] || fail "CSV has no data rows"
echo "ok: CSV header + $rows rows"

awk -F, 'NR > 1 {
    if ($2 < $1) { printf "receive < send at line %d: %s\n", NR, $0; bad = 1; exit 1 }
    if ($2 - $1 >= 5e9) { printf "rtt >= 5e9 ns at line %d: %s\n", NR, $0; bad = 1; exit 1 }
}' "$CSV" || fail "CSV sanity check failed"
echo "ok: all receive >= send, all RTT < 5e9 ns"

# --- lossless: Valkey lists retain everything, so every sent message must
# come back (in async mode this also proves the final settle delivered the
# buffered tail).
sent="$(grep -o 'sent [0-9]*' "$WORKDIR/send.log" | awk '{print $2}')"
[ -n "$sent" ] || fail "could not parse sender count: $(cat "$WORKDIR/send.log")"
[ "$rows" -eq "$sent" ] || fail "receiver got $rows rows, sender sent $sent messages"
echo "ok: receiver rows == $sent sent messages"

echo "rtt_integration ($MODE): all checks passed"
