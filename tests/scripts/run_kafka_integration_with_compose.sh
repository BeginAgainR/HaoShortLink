#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TEST_HOST="${HAOHTTP_TEST_HOST:-}"
TEST_WORKDIR="${HAOHTTP_TEST_WORKDIR:-${ROOT_DIR}}"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
KAFKA_IMAGE="${SHORTLINK_KAFKA_IMAGE:-apache/kafka:4.3.1}"
KAFKA_UI_IMAGE="${SHORTLINK_KAFKA_UI_IMAGE:-ghcr.io/kafbat/kafka-ui:v1.5.0}"
PROMETHEUS_IMAGE="${SHORTLINK_PROMETHEUS_IMAGE:-prom/prometheus:v3.13.0}"
TOPIC="${HAOHTTP_KAFKA_TOPIC:-hao-shortlink.access-events.v1}"
DLQ_TOPIC="${HAOHTTP_KAFKA_DLQ_TOPIC:-hao-shortlink.access-events.dlq.v1}"

if [[ -n "${TEST_HOST}" ]]; then
    EXTERNAL_HOST="${SHORTLINK_KAFKA_EXTERNAL_HOST:-docker.orb.internal}"
    TEST_BOOTSTRAP="${HAOHTTP_KAFKA_BOOTSTRAP_SERVERS:-docker.orb.internal:19092}"
    TEST_MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
else
    EXTERNAL_HOST="${SHORTLINK_KAFKA_EXTERNAL_HOST:-127.0.0.1}"
    TEST_BOOTSTRAP="${HAOHTTP_KAFKA_BOOTSTRAP_SERVERS:-127.0.0.1:19092}"
    TEST_MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-127.0.0.1}"
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
            mysql kafka kafka_topic_init kafka_ui >&2 || true
    fi
    docker compose -f compose.yaml -f compose.kafka.yaml rm -sf \
        kafka_ui kafka_topic_init kafka >/dev/null 2>&1 || true
}

wait_for_healthy()
{
    local container_name="$1"
    local label="$2"
    for _ in {1..60}; do
        if [[ "$(docker inspect -f '{{.State.Health.Status}}' "${container_name}" 2>/dev/null || true)" == "healthy" ]]; then
            echo "PASS: ${label} is healthy"
            return 0
        fi
        sleep 1
    done
    fail "${label} did not become healthy"
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker info >/dev/null 2>&1 || fail "Docker is not available"

docker run --rm \
    --entrypoint promtool \
    -v "${ROOT_DIR}/deploy/prometheus/prometheus.kafka.yml:/etc/prometheus/prometheus.yml:ro" \
    "${PROMETHEUS_IMAGE}" \
    check config /etc/prometheus/prometheus.yml >/dev/null
echo "PASS: Kafka overlay Prometheus configuration"

cd "${ROOT_DIR}"
docker compose -f compose.yaml -f compose.kafka.yaml build shortlink_server

SHORTLINK_KAFKA_IMAGE="${KAFKA_IMAGE}" \
SHORTLINK_KAFKA_UI_IMAGE="${KAFKA_UI_IMAGE}" \
SHORTLINK_KAFKA_EXTERNAL_HOST="${EXTERNAL_HOST}" \
docker compose -f compose.yaml -f compose.kafka.yaml up -d mysql kafka kafka_topic_init kafka_ui
wait_for_healthy "hao-shortlink-mysql" "MySQL"
SHORTLINK_MYSQL_IMAGE="${SHORTLINK_MYSQL_IMAGE:-mysql:8.0}" \
docker compose -f compose.yaml -f compose.kafka.yaml run --rm schema_migrate
echo "PASS: schema migration completed before Kafka integration tests"

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

dlq_description="$(docker exec hao-shortlink-kafka /opt/kafka/bin/kafka-topics.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --describe \
    --topic "${DLQ_TOPIC}")"
[[ "${dlq_description}" == *"PartitionCount: 3"* ]] ||
    fail "Kafka DLQ topic does not have 3 partitions: ${dlq_description}"

dlq_config="$(docker exec hao-shortlink-kafka /opt/kafka/bin/kafka-configs.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --entity-type topics \
    --entity-name "${DLQ_TOPIC}" \
    --describe)"
[[ "${dlq_config}" == *"retention.ms=2592000000"* ]] ||
    fail "Kafka DLQ topic retention is not 30 days: ${dlq_config}"
echo "PASS: Kafka DLQ topic runtime configuration"

ui_host_ip="$(docker inspect -f \
    '{{(index (index .NetworkSettings.Ports "8080/tcp") 0).HostIp}}' \
    hao-shortlink-kafka-ui)"
[[ "${ui_host_ip}" == "127.0.0.1" ]] ||
    fail "Kafka UI must bind only to localhost, got ${ui_host_ip}"
echo "PASS: Kafka UI binds only to localhost"

if [[ -n "${TEST_HOST}" ]]; then
    ssh "${TEST_HOST}" \
        "cd '${TEST_WORKDIR}' && HAOHTTP_BUILD_DIR='${BUILD_DIR}' HAOHTTP_KAFKA_BOOTSTRAP_SERVERS='${TEST_BOOTSTRAP}' HAOHTTP_MYSQL_HOST='${TEST_MYSQL_HOST}' bash tests/scripts/kafka_integration_test.sh"
else
    HAOHTTP_BUILD_DIR="${BUILD_DIR}" \
    HAOHTTP_KAFKA_BOOTSTRAP_SERVERS="${TEST_BOOTSTRAP}" \
    HAOHTTP_MYSQL_HOST="${TEST_MYSQL_HOST}" \
    bash tests/scripts/kafka_integration_test.sh
fi

bash tests/scripts/kafka_broker_recovery_test.sh
bash tests/scripts/access_statistics_mysql_recovery_test.sh
bash tests/scripts/access_statistics_dlq_failure_test.sh
bash tests/scripts/access_statistics_replay_test.sh

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
