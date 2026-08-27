# Shared test fixture: Pulsar standalone broker in docker.
#
# Ports default to 26650/28080 to avoid clashing with a system Pulsar on
# 6650/8080. If a broker is already healthy on PULSAR_ADMIN_URL it is reused
# (and left running); otherwise a fresh `--rm` container is started and
# removed on exit via trap.
#
# Sourcing scripts must call `start_broker` and install a trap that calls
# `stop_broker`.

PULSAR_PORT="${PULSAR_PORT:-26650}"
PULSAR_ADMIN_PORT="${PULSAR_ADMIN_PORT:-28080}"
PULSAR_URL="${PULSAR_URL:-pulsar://127.0.0.1:${PULSAR_PORT}}"
PULSAR_ADMIN_URL="${PULSAR_ADMIN_URL:-http://127.0.0.1:${PULSAR_ADMIN_PORT}}"
PULSAR_IMAGE="${PULSAR_IMAGE:-apachepulsar/pulsar:4.0.5}"

BROKER_CONTAINER=""

broker_healthy() {
    curl -sf -m 3 "${PULSAR_ADMIN_URL}/admin/v2/brokers/health" >/dev/null 2>&1
}

start_broker() {
    if broker_healthy; then
        echo "Reusing running Pulsar broker at ${PULSAR_ADMIN_URL}"
        return 0
    fi
    BROKER_CONTAINER="pulsar-bench-test-$$-${RANDOM}"
    echo "Starting Pulsar standalone broker (container ${BROKER_CONTAINER})"
    docker run --rm -d --name "${BROKER_CONTAINER}" \
        -p "${PULSAR_PORT}:6650" -p "${PULSAR_ADMIN_PORT}:8080" \
        "${PULSAR_IMAGE}" bin/pulsar standalone -nfw -nss >/dev/null
    # Standalone startup is slow; allow ~120s.
    for _ in $(seq 1 120); do
        if broker_healthy; then
            echo "Pulsar broker is healthy"
            return 0
        fi
        sleep 1
    done
    echo "FAIL: Pulsar broker did not become healthy within 120s" >&2
    docker logs "${BROKER_CONTAINER}" 2>&1 | tail -30 >&2 || true
    exit 1
}

stop_broker() {
    if [ -n "${BROKER_CONTAINER}" ]; then
        docker rm -f "${BROKER_CONTAINER}" >/dev/null 2>&1 || true
        BROKER_CONTAINER=""
    fi
}
