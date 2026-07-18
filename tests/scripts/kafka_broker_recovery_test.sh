#!/usr/bin/env bash

set -euo pipefail

SERVER_IMAGE="${SHORTLINK_SERVER_IMAGE:-hao-shortlink-server:dev}"
SERVER_CONTAINER="${HAOHTTP_KAFKA_RECOVERY_SERVER_CONTAINER:-hao-shortlink-kafka-recovery-server}"
KAFKA_CONTAINER="${HAOHTTP_KAFKA_CONTAINER:-hao-shortlink-kafka}"
NETWORK="${HAOHTTP_KAFKA_NETWORK:-hao-shortlink-dev_default}"
PORT="${HAOHTTP_KAFKA_RECOVERY_PORT:-18086}"
BASE_URL="http://127.0.0.1:${PORT}"
SERVER_LOG=""

cleanup()
{
    docker rm -f "${SERVER_CONTAINER}" >/dev/null 2>&1 || true
    docker start "${KAFKA_CONTAINER}" >/dev/null 2>&1 || true
}

fail()
{
    echo "FAIL: $*" >&2
    docker logs --tail=120 "${SERVER_CONTAINER}" >&2 || true
    exit 1
}

expect_eq()
{
    local actual="$1"
    local expected="$2"
    local message="$3"
    if [[ "${actual}" != "${expected}" ]]; then
        fail "${message}: expected ${expected}, got ${actual}"
    fi
}

metric_value()
{
    local metric_line="$1"
    curl -fsS "${BASE_URL}/metrics" | awk -v line="${metric_line}" '$0 ~ line { print $NF; exit }'
}

wait_for_kafka()
{
    for _ in {1..60}; do
        if [[ "$(docker inspect -f '{{.State.Health.Status}}' "${KAFKA_CONTAINER}" 2>/dev/null || true)" == "healthy" ]]; then
            return 0
        fi
        sleep 1
    done
    fail "Kafka did not become healthy after restart"
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"
docker inspect "${KAFKA_CONTAINER}" >/dev/null 2>&1 || fail "Kafka container is not running"
docker image inspect "${SERVER_IMAGE}" >/dev/null 2>&1 || fail "server image ${SERVER_IMAGE} is missing"

docker rm -f "${SERVER_CONTAINER}" >/dev/null 2>&1 || true
docker run -d \
    --name "${SERVER_CONTAINER}" \
    --network "${NETWORK}" \
    -p "127.0.0.1:${PORT}:8080" \
    "${SERVER_IMAGE}" \
    ./shortlink_server apps/shortlink_server/config/server.kafka.test.container.conf.example \
    >/dev/null

for _ in {1..80}; do
    if ! docker inspect "${SERVER_CONTAINER}" >/dev/null 2>&1; then
        fail "recovery test server exited before becoming healthy"
    fi
    if curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1 || fail "recovery test server did not become healthy"

body="$(curl -fsS -H 'Content-Type: application/json' \
    -d '{"url":"https://example.com/kafka-recovery"}' \
    "${BASE_URL}/api/short-links")"
code="$(printf '%s' "${body}" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')"
[[ -n "${code}" ]] || fail "recovery fixture response did not contain a code"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' "${BASE_URL}/s/${code}")" \
    "302" "healthy Kafka redirect"

for _ in {1..50}; do
    if [[ "$(metric_value 'haohttp_shortlink_access_event_delivery_total\{result="success"\}')" -ge 1 ]]; then
        break
    fi
    sleep 0.1
done
[[ "$(metric_value 'haohttp_shortlink_access_event_delivery_total\{result="success"\}')" -ge 1 ]] ||
    fail "initial access event was not delivered"
echo "PASS: baseline event delivered before broker stop"

docker stop --time 10 "${KAFKA_CONTAINER}" >/dev/null
for request in {1..20}; do
    expect_eq "$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' "${BASE_URL}/s/${code}")" \
        "302" "redirect during broker outage ${request}"
done
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' "${BASE_URL}/api/health/ready")" \
    "200" "readiness during broker outage"
sleep 2
outage_log="$(docker logs "${SERVER_CONTAINER}" 2>&1 || true)"
[[ "${outage_log}" == *"event=kafka_access_event stage=client result=failure"* ]] ||
    fail "broker outage did not produce an observable Kafka client failure"
echo "PASS: broker stop does not change redirect or readiness semantics"

docker start "${KAFKA_CONTAINER}" >/dev/null
wait_for_kafka
for _ in {1..30}; do
    expect_eq "$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' "${BASE_URL}/s/${code}")" \
        "302" "redirect after broker recovery"
    if [[ "$(metric_value 'haohttp_shortlink_access_event_delivery_total\{result="success"\}')" -ge 2 ]]; then
        break
    fi
    sleep 0.2
done
[[ "$(metric_value 'haohttp_shortlink_access_event_delivery_total\{result="success"\}')" -ge 2 ]] ||
    fail "producer did not recover delivery after broker restart"
echo "PASS: producer reconnects after broker restart"

docker stop --time 5 "${SERVER_CONTAINER}" >/dev/null
SERVER_LOG="$(docker logs "${SERVER_CONTAINER}" 2>&1 || true)"
[[ "${SERVER_LOG}" == *"event=server_shutdown"* ]] || fail "container SIGTERM did not trigger graceful shutdown"
[[ "${SERVER_LOG}" == *"event=kafka_access_event_producer stage=shutdown"* ]] ||
    fail "container shutdown did not run producer flush"
echo "PASS: container shutdown runs bounded producer flush"

echo "Kafka broker recovery test passed"
