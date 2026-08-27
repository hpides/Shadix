#!/usr/bin/env bash
# Tier 1: CLI behavior. No broker required.
#
# Usage: cli_test.sh [RTT_BIN] [TPUT_BIN]
# Defaults to ../build/{rtt,throughput} relative to this script.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RTT="${1:-${RTT_BIN:-$SCRIPT_DIR/../build/rtt}}"
TPUT="${2:-${TPUT_BIN:-$SCRIPT_DIR/../build/throughput}}"

fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok: $*"; }

[ -x "$RTT" ] || fail "rtt binary not found/executable at $RTT"
[ -x "$TPUT" ] || fail "throughput binary not found/executable at $TPUT"

# Port 1 is never a Valkey server: every case below must fail before
# connecting, so a broker contact would show up as a different error.
NOBROKER=redis://127.0.0.1:1

# --- 1. usage output on no args (both binaries), nonzero exit -----------------
out=$("$RTT" 2>&1); rc=$?
[ "$rc" -ne 0 ] || fail "rtt with no args should exit nonzero (got $rc)"
echo "$out" | grep -qi "subcommand" || fail "rtt usage output missing 'Subcommands' (got: $out)"

out=$("$TPUT" 2>&1); rc=$?
[ "$rc" -ne 0 ] || fail "throughput with no args should exit nonzero (got $rc)"
echo "$out" | grep -qi "subcommand" || fail "throughput usage output missing 'Subcommands' (got: $out)"
ok "usage on no args"

# --- 2. unknown transport -> exit 1, message names the accepted value ---------
out=$("$RTT" --url $NOBROKER --transport bogus -s 8 init 2>&1); rc=$?
[ "$rc" -eq 1 ] || fail "rtt unknown transport should exit 1 (got $rc: $out)"
echo "$out" | grep -q "list" || fail "rtt transport error must mention 'list' (got: $out)"

out=$("$TPUT" init --url $NOBROKER --transport bogus -s 8 2>&1); rc=$?
[ "$rc" -eq 1 ] || fail "throughput unknown transport should exit 1 (got $rc: $out)"
echo "$out" | grep -q "list" || fail "throughput transport error must mention 'list' (got: $out)"
ok "unknown transport rejected"

# --- 3. rtt send rejects --item-size < 8 --------------------------------------
out=$("$RTT" --url $NOBROKER --transport list -s 4 \
    send --id 0 --concurrency 1 --duration 1s --load 0.001 2>&1); rc=$?
[ "$rc" -eq 1 ] || fail "rtt --item-size 4 should exit 1 (got $rc: $out)"
echo "$out" | grep -q "item-size" || fail "rtt item-size error must mention 'item-size' (got: $out)"
ok "rtt rejects item-size < 8"

# --- 4. throughput start-time prints an integer > 0 ---------------------------
out=$("$TPUT" start-time 100ms 2>&1); rc=$?
[ "$rc" -eq 0 ] || fail "throughput start-time 100ms should exit 0 (got $rc: $out)"
echo "$out" | grep -qE '^[0-9]+$' || fail "start-time output not an integer (got: $out)"
[ "$out" -gt 0 ] || fail "start-time output not > 0 (got: $out)"
ok "start-time prints integer > 0"

# --- 5. produce: invalid --send-mode -> exit 1 (validated before broker contact)
out=$("$TPUT" produce --url $NOBROKER --transport list -s 8 \
    --id 0 --concurrency 1 --batches 1 --batch-size 1 --start-time 0 \
    --send-mode bogus 2>&1); rc=$?
[ "$rc" -eq 1 ] || fail "produce invalid --send-mode should exit 1 (got $rc: $out)"
echo "$out" | grep -q "sync" || fail "produce send-mode error must mention 'sync' (got: $out)"
echo "$out" | grep -q "async" || fail "produce send-mode error must mention 'async' (got: $out)"
ok "produce rejects invalid send-mode"

# --- 6. rtt send/server: invalid --send-mode -> exit 1, names sync/async ------
out=$("$RTT" --url $NOBROKER --transport list -s 8 \
    send --id 0 --concurrency 1 --duration 1s --load 0.001 --send-mode bogus 2>&1); rc=$?
[ "$rc" -eq 1 ] || fail "rtt send invalid --send-mode should exit 1 (got $rc: $out)"
echo "$out" | grep -q "sync" || fail "rtt send send-mode error must mention 'sync' (got: $out)"
echo "$out" | grep -q "async" || fail "rtt send send-mode error must mention 'async' (got: $out)"

out=$("$RTT" --url $NOBROKER --transport list -s 8 \
    server --id 0 --concurrency 1 --duration 1s --send-mode bogus 2>&1); rc=$?
[ "$rc" -eq 1 ] || fail "rtt server invalid --send-mode should exit 1 (got $rc: $out)"
echo "$out" | grep -q "sync" || fail "rtt server send-mode error must mention 'sync' (got: $out)"
echo "$out" | grep -q "async" || fail "rtt server send-mode error must mention 'async' (got: $out)"
ok "rtt send/server reject invalid send-mode"

echo "cli_test: all checks passed"
