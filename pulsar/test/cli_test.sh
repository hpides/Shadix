#!/usr/bin/env bash
# Tier 1: CLI validation tests. No broker required.
#
# Checks:
#   1. usage output on no args (both binaries)
#   2. unknown --transport -> exit 1, message names accepted values
#   3. rtt --item-size < 8 -> exit 1
#   4. `throughput start-time 100ms` prints an integer > 0
#   5. invalid --send-mode -> exit 1
#   6. rtt send/server with --send-mode bogus -> exit 1, message names
#      sync/async (validated before any broker contact: the URL points at a
#      closed port, so a connection attempt would fail differently)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../build}"
RTT="$BUILD_DIR/rtt"
TPUT="$BUILD_DIR/throughput"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

[ -x "$RTT" ] || fail "rtt binary not found at $RTT"
[ -x "$TPUT" ] || fail "throughput binary not found at $TPUT"

# 1. usage on no args
out=$("$RTT" 2>&1) && fail "rtt with no args should exit nonzero"
echo "$out" | grep -q "Subcommands" || fail "rtt usage output missing (got: $out)"
pass "rtt prints usage on no args"

out=$("$TPUT" 2>&1) && fail "throughput with no args should exit nonzero"
echo "$out" | grep -q "Subcommands" || fail "throughput usage output missing (got: $out)"
pass "throughput prints usage on no args"

# 2. unknown transport -> exit 1, names accepted values
out=$("$RTT" --transport bogus -s 8 init 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "rtt unknown transport: expected exit 1, got $rc"
echo "$out" | grep -q "persistent" || fail "rtt unknown-transport error must mention 'persistent' (got: $out)"
echo "$out" | grep -q "non-persistent" || fail "rtt unknown-transport error must mention 'non-persistent' (got: $out)"
pass "rtt rejects unknown transport with accepted values"

out=$("$TPUT" init --transport bogus -s 8 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "throughput unknown transport: expected exit 1, got $rc"
echo "$out" | grep -q "persistent" || fail "throughput unknown-transport error must mention 'persistent' (got: $out)"
echo "$out" | grep -q "non-persistent" || fail "throughput unknown-transport error must mention 'non-persistent' (got: $out)"
pass "throughput rejects unknown transport with accepted values"

# 3. rtt --item-size 4 -> exit 1
out=$("$RTT" --transport persistent --item-size 4 init 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "rtt --item-size 4: expected exit 1, got $rc"
echo "$out" | grep -qi "item-size" || fail "rtt item-size error must mention item-size (got: $out)"
pass "rtt rejects --item-size < 8"

# 4. throughput start-time prints an integer > 0
out=$("$TPUT" start-time 100ms)
rc=$?
[ "$rc" -eq 0 ] || fail "throughput start-time: expected exit 0, got $rc"
[[ "$out" =~ ^[0-9]+$ ]] || fail "throughput start-time output not an integer: $out"
[ "$out" -gt 0 ] || fail "throughput start-time output not > 0: $out"
pass "throughput start-time prints integer > 0"

# 5. invalid --send-mode -> exit 1
out=$("$TPUT" produce --transport persistent -s 8 --id 0 --concurrency 1 \
      --batches 1 --batch-size 1 --start-time 1 --send-mode bogus 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "invalid --send-mode: expected exit 1, got $rc"
echo "$out" | grep -q "sync" || fail "send-mode error must mention accepted values (got: $out)"
pass "throughput rejects invalid --send-mode"

# 6. rtt send/server with --send-mode bogus -> exit 1, names accepted values
out=$("$RTT" --url pulsar://127.0.0.1:1 --transport persistent -s 8 \
      send --id 0 --concurrency 1 --duration 1s --load 0.001 --send-mode bogus 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "rtt send invalid --send-mode: expected exit 1, got $rc (got: $out)"
echo "$out" | grep -q "sync" || fail "rtt send send-mode error must mention 'sync' (got: $out)"
echo "$out" | grep -q "async" || fail "rtt send send-mode error must mention 'async' (got: $out)"
pass "rtt send rejects invalid --send-mode with accepted values"

out=$("$RTT" --url pulsar://127.0.0.1:1 --transport persistent -s 8 \
      server --id 0 --concurrency 1 --duration 1s --send-mode bogus 2>&1)
rc=$?
[ "$rc" -eq 1 ] || fail "rtt server invalid --send-mode: expected exit 1, got $rc (got: $out)"
echo "$out" | grep -q "sync" || fail "rtt server send-mode error must mention 'sync' (got: $out)"
echo "$out" | grep -q "async" || fail "rtt server send-mode error must mention 'async' (got: $out)"
pass "rtt server rejects invalid --send-mode with accepted values"

echo "cli_test: ALL PASS"
