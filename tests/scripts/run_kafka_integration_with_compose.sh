#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_HOST="${HAOHTTP_TEST_HOST:-}"
TEST_WORKDIR="${HAOHTTP_TEST_WORKDIR:-${ROOT_DIR}}"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
KAFKA_IMAGE="${SHORTLINK_KAFKA_IMAGE:-apache/kafka:4.3.1}"
KAFKA_UI_IMAGE="${SHORTLINK_KAFKA_UI_IMAGE:-ghcr.io/kafbat/kafka-ui:v1.5.0}"
TOPIC="${HAOHTTP_KAFKA_TOPIC:-hao-shortlink.access-events.v1}"

if [[ -n "${TEST_HOST}" ]]; then
    EXTERNAL_HOST="${SHORTLINK_KAFKA_EXTERNAL_HOST:-docker.orb.internal}"
    TEST_BOOTSTRAP="${HAOHTTP_KAFKA_BOOTSTRAP_SERVERS:-docker.orb.internal:19092}"
else
    EXTERNAL_HOST="${SHORTLINK_KAFKA_EXTERNAL_HOST:-127.0.0.1}"
    TEST_BOOTSTRAP="${HAOHTTP_KAFKA_BOOTSTRAP_SERVERS:-127.0.0.1:19092}"
fi

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

cleanup()
{
    local status="$?"
    if [[ "${status}" -ne 0 ]]; then
        docker compose -f compose.yaml -f compose.kafka.yaml logs --tail=150 \
            kafka kafka_topic_init kafka_ui >&2 || true
    fi
    docker compose -f compose.yaml -f compose.kafka.yaml rm -sf \
        kafka_ui kafka_topic_init kafka >/dev/null 2>&1 || true
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker info >/dev/null 2>&1 || fail "Docker is not available"

cd "${ROOT_DIR}"
docker compose -f compose.yaml -f compose.kafka.yaml build shortlink_server

SHORTLINK_KAFKA_IMAGE="${KAFKA_IMAGE}" \
SHORTLINK_KAFKA_UI_IMAGE="${KAFKA_UI_IMAGE}" \
SHORTLINK_KAFKA_EXTERNAL_HOST="${EXTERNAL_HOST}" \
docker compose -f compose.yaml -f compose.kafka.yaml up -d kafka kafka_topic_init kafka_ui

topic_description="$(docker exec hao-shortlink-kafka /opt/kafka/bin/kafka-topics.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --describe \
    --topic "${TOPIC}")"
[[ "${topic_description}" == *"PartitionCount: 3"* ]] ||
    fail "Kafka topic does not have 3 partitions: ${topic_description}"
[[ "${topic_description}" == *"ReplicationFactor: 1"* ]] ||
    fail "Kafka topic does not have replication factor 1: ${topic_description}"

topic_config="$(docker exec hao-shortlink-kafka /opt/kafka/bin/kafka-configs.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --entity-type topics \
    --entity-name "${TOPIC}" \
    --describe)"
[[ "${topic_config}" == *"cleanup.policy=delete"* ]] ||
    fail "Kafka topic cleanup policy is not delete: ${topic_config}"
[[ "${topic_config}" == *"retention.ms=604800000"* ]] ||
    fail "Kafka topic retention is not 7 days: ${topic_config}"
echo "PASS: Kafka topic runtime configuration"

ui_host_ip="$(docker inspect -f \
    '{{(index (index .NetworkSettings.Ports "8080/tcp") 0).HostIp}}' \
    hao-shortlink-kafka-ui)"
[[ "${ui_host_ip}" == "127.0.0.1" ]] ||
    fail "Kafka UI must bind only to localhost, got ${ui_host_ip}"
echo "PASS: Kafka UI binds only to localhost"

if [[ -n "${TEST_HOST}" ]]; then
    ssh "${TEST_HOST}" \
        "cd '${TEST_WORKDIR}' && HAOHTTP_BUILD_DIR='${BUILD_DIR}' HAOHTTP_KAFKA_BOOTSTRAP_SERVERS='${TEST_BOOTSTRAP}' bash tests/scripts/kafka_integration_test.sh"
else
    HAOHTTP_BUILD_DIR="${BUILD_DIR}" \
    HAOHTTP_KAFKA_BOOTSTRAP_SERVERS="${TEST_BOOTSTRAP}" \
    bash tests/scripts/kafka_integration_test.sh
fi

bash tests/scripts/kafka_broker_recovery_test.sh

for _ in {1..60}; do
    if curl -fsS http://127.0.0.1:18081/actuator/health 2>/dev/null | grep -q '"status":"UP"'; then
        echo "PASS: Kafka UI is healthy"
        echo "Compose-backed Kafka integration test passed"
        exit 0
    fi
    sleep 1
done
docker compose -f compose.yaml -f compose.kafka.yaml logs --tail=100 kafka kafka_topic_init kafka_ui >&2 || true
fail "Kafka UI health endpoint is not UP"
