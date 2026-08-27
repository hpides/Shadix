# Shared broker fixture for the integration tests. Source this file.
#
# Starts a dedicated nats-server (JetStream enabled) on NATS_TEST_PORT
# (default 24222, chosen to avoid clashing with a system nats-server on 4222;
# CTest overrides it per test — see CMakeLists.txt — so `ctest -j` is safe)
# with a throwaway JetStream store dir, and installs a trap that kills the
# server and removes all temp dirs on exit.
#
# Provides: $NATS_URL, $WORKDIR, and fail().

NATS_TEST_PORT="${NATS_TEST_PORT:-24222}"
NATS_URL="nats://127.0.0.1:${NATS_TEST_PORT}"
NATS_SERVER_BIN="${NATS_SERVER_BIN:-nats-server}"

fail() { echo "FAIL: $*" >&2; exit 1; }

command -v "$NATS_SERVER_BIN" >/dev/null 2>&1 \
    || fail "nats-server not found (run inside 'nix develop .#benchmarks' or set NATS_SERVER_BIN)"

STORE_DIR="$(mktemp -d)"
WORKDIR="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    status=$?
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$STORE_DIR" "$WORKDIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

# Refuse to run if something already listens on the port: the readiness poll
# below cannot tell our server from a stranger's, and sharing a broker makes
# tests cross-consume each other's messages. (CTest gives every integration
# test its own port via the ENVIRONMENT property in CMakeLists.txt.)
if (exec 3<>"/dev/tcp/127.0.0.1/$NATS_TEST_PORT") 2>/dev/null; then
    fail "port $NATS_TEST_PORT is already in use; set NATS_TEST_PORT to a free port"
fi

"$NATS_SERVER_BIN" -js -p "$NATS_TEST_PORT" -sd "$STORE_DIR" \
    > "$WORKDIR/nats-server.log" 2>&1 &
SERVER_PID=$!

# Readiness: poll the client port.
ready=""
for _ in $(seq 1 100); do
    # Probe in a subshell so the fd is closed when it exits.
    if (exec 3<>"/dev/tcp/127.0.0.1/$NATS_TEST_PORT") 2>/dev/null; then
        ready=1
        break
    fi
    kill -0 "$SERVER_PID" 2>/dev/null \
        || fail "nats-server died during startup: $(cat "$WORKDIR/nats-server.log")"
    sleep 0.1
done
[ -n "$ready" ] || fail "nats-server did not become ready on port $NATS_TEST_PORT"
