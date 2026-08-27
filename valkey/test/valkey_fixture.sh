# Shared broker fixture for the integration tests. Source this file.
#
# Starts a dedicated Valkey server in docker (valkey/valkey:9.1.0, the
# version the sweeps ran against) on VALKEY_TEST_PORT (default 26379, chosen
# to avoid clashing with a system valkey-server on 6379; CTest overrides it
# per test — see CMakeLists.txt — so `ctest -j` is safe) and installs a trap
# that removes the container and the temp dir on exit.
#
# Provides: $VALKEY_URL, $WORKDIR, and fail().

VALKEY_TEST_PORT="${VALKEY_TEST_PORT:-26379}"
VALKEY_URL="redis://127.0.0.1:${VALKEY_TEST_PORT}"
VALKEY_IMAGE="${VALKEY_IMAGE:-valkey/valkey:9.1.0}"

fail() { echo "FAIL: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 \
    || fail "docker not found (the integration tests run Valkey in a container)"

WORKDIR="$(mktemp -d)"
CONTAINER=""

cleanup() {
    status=$?
    if [ -n "$CONTAINER" ]; then
        docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORKDIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

# Refuse to run if something already listens on the port: the readiness poll
# below cannot tell our server from a stranger's, and sharing a broker makes
# tests cross-consume each other's lists. (CTest gives every integration test
# its own port via the ENVIRONMENT property in CMakeLists.txt.)
if (exec 3<>"/dev/tcp/127.0.0.1/$VALKEY_TEST_PORT") 2>/dev/null; then
    fail "port $VALKEY_TEST_PORT is already in use; set VALKEY_TEST_PORT to a free port"
fi

# Pull explicitly so a first-time download does not interleave its progress
# output with the test output.
if ! docker image inspect "$VALKEY_IMAGE" >/dev/null 2>&1; then
    docker pull "$VALKEY_IMAGE" > "$WORKDIR/docker-pull.log" 2>&1 \
        || fail "could not pull $VALKEY_IMAGE: $(cat "$WORKDIR/docker-pull.log")"
fi

CONTAINER="valkey-bench-test-$$-${RANDOM}"
docker run --rm -d --name "$CONTAINER" -p "127.0.0.1:${VALKEY_TEST_PORT}:6379" \
    "$VALKEY_IMAGE" > "$WORKDIR/docker-run.log" 2>&1 \
    || fail "docker run failed: $(cat "$WORKDIR/docker-run.log")"

# Readiness: PING through the container (the published port accepts TCP
# connections before the server inside is listening).
ready=""
for _ in $(seq 1 100); do
    if docker exec "$CONTAINER" valkey-cli ping 2>/dev/null | grep -q PONG; then
        ready=1
        break
    fi
    [ "$(docker inspect -f '{{.State.Running}}' "$CONTAINER" 2>/dev/null)" = "true" ] \
        || fail "valkey container died during startup: $(docker logs "$CONTAINER" 2>&1 | tail -20)"
    sleep 0.1
done
[ -n "$ready" ] || fail "valkey did not become ready on port $VALKEY_TEST_PORT"
