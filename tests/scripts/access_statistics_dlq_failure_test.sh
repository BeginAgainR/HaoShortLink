#!/usr/bin/env bash

set -euo pipefail

SERVER_IMAGE="${SHORTLINK_SERVER_IMAGE:-hao-shortlink-server:dev}"
CONSUMER_CONTAINER="${HAOHTTP_STATISTICS_DLQ_CONSUMER:-hao-shortlink-statistics-dlq-test-consumer}"
KAFKA_CONTAINER="${HAOHTTP_KAFKA_CONTAINER:-hao-shortlink-kafka}"
NETWORK="${HAOHTTP_KAFKA_NETWORK:-hao-shortlink-dev_default}"
DLQ_TOPIC="${HAOHTTP_KAFKA_DLQ_TOPIC:-hao-shortlink.access-events.dlq.v1}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-statistics-dlq-failure.XXXXXX")"
BAD_CONFIG="${TMP_DIR}/bad-dlq.conf"
GOOD_CONFIG="${TMP_DIR}/good-dlq.conf"
RUN_ID="$(date +%s)-${RANDOM}"
SOURCE_TOPIC="hao-shortlink.access-events.dlq-test-${RUN_ID}"
MISSING_DLQ_TOPIC="hao-shortlink.access-events.missing-dlq-${RUN_ID}"
GROUP_ID="hao-shortlink-statistics-dlq-test-${RUN_ID}"

cleanup()
{
    docker rm -f "${CONSUMER_CONTAINER}" >/dev/null 2>&1 || true
    docker exec "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-topics.sh \
        --bootstrap-server 127.0.0.1:29092 \
        --delete --topic "${SOURCE_TOPIC}" >/dev/null 2>&1 || true
    rm -rf "${TMP_DIR}"
}

fail()
{
    echo "FAIL: $*" >&2
    docker logs --tail=120 "${CONSUMER_CONTAINER}" >&2 || true
    exit 1
}

write_config()
{
    local path="$1"
    local dlq_topic="$2"
    cat > "${path}" <<EOF
consumer.bootstrap_servers=kafka:29092
consumer.topic=${SOURCE_TOPIC}
consumer.group_id=${GROUP_ID}
consumer.client_id=hao-shortlink-statistics-dlq-test-${RUN_ID}
consumer.auto_offset_reset=earliest
consumer.poll_timeout_ms=100
consumer.session_timeout_ms=6000
consumer.dlq_topic=${dlq_topic}
consumer.dlq_client_id=hao-shortlink-statistics-dlq-test-publisher-${RUN_ID}
consumer.dlq_message_timeout_ms=1000
consumer.dlq_delivery_timeout_ms=1500
consumer.processing_max_attempts=2
consumer.retry_initial_ms=100
consumer.retry_max_ms=100
consumer.offset_commit_max_attempts=3
mysql.host=tcp://mysql:3306
mysql.user=hao_shortlink
mysql.password=hao_shortlink
mysql.database=hao_shortlink
EOF
}

start_consumer()
{
    local config="$1"
    docker rm -f "${CONSUMER_CONTAINER}" >/dev/null 2>&1 || true
    docker run -d \
        --name "${CONSUMER_CONTAINER}" \
        --network "${NETWORK}" \
        -v "${config}:/tmp/consumer.conf:ro" \
        "${SERVER_IMAGE}" \
        ./shortlink_event_consumer /tmp/consumer.conf \
        >/dev/null
    for _ in {1..40}; do
        local status
        status="$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}" 2>/dev/null || true)"
        if [[ "${status}" == "running" ]]; then
            sleep 1
            [[ "$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}")" == "running" ]] && return 0
        fi
        [[ "${status}" != "exited" ]] || fail "consumer exited before polling"
        sleep 0.1
    done
    fail "consumer did not start"
}

topic_end_sum()
{
    docker exec "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-get-offsets.sh \
        --bootstrap-server 127.0.0.1:29092 \
        --topic "$1" \
        --time -1 2>/dev/null | awk -F: '{sum += $3} END {print sum + 0}'
}

source_committed_offset()
{
    docker exec "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-consumer-groups.sh \
        --bootstrap-server 127.0.0.1:29092 \
        --describe --group "${GROUP_ID}" 2>/dev/null | \
        awk -v topic="${SOURCE_TOPIC}" '$2 == topic && $3 == 0 {print $4; exit}'
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker inspect "${KAFKA_CONTAINER}" >/dev/null 2>&1 || fail "Kafka container is missing"
docker image inspect "${SERVER_IMAGE}" >/dev/null 2>&1 || fail "server image is missing"

docker exec "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-topics.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --create --topic "${SOURCE_TOPIC}" \
    --partitions 1 --replication-factor 1 \
    --config cleanup.policy=delete --config retention.ms=3600000 \
    >/dev/null
write_config "${BAD_CONFIG}" "${MISSING_DLQ_TOPIC}"
write_config "${GOOD_CONFIG}" "${DLQ_TOPIC}"

dlq_before="$(topic_end_sum "${DLQ_TOPIC}")"
start_consumer "${BAD_CONFIG}"
docker exec -i "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-console-producer.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --topic "${SOURCE_TOPIC}" \
    --reader-property parse.key=true \
    --reader-property key.separator='|' \
    <<< 'bad-json|not-json'

for _ in {1..40}; do
    [[ "$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}" 2>/dev/null || true)" == "exited" ]] && break
    sleep 1
done
[[ "$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}")" == "exited" ]] ||
    fail "consumer did not fail when DLQ delivery was unavailable"
[[ "$(docker inspect -f '{{.State.ExitCode}}' "${CONSUMER_CONTAINER}")" != "0" ]] ||
    fail "consumer should exit non-zero after DLQ retry exhaustion"
[[ "$(topic_end_sum "${DLQ_TOPIC}")" == "${dlq_before}" ]] ||
    fail "unavailable DLQ test unexpectedly wrote the configured DLQ"
echo "PASS: unavailable DLQ exhausts bounded retries without a successful delivery"

start_consumer "${GOOD_CONFIG}"
for _ in {1..100}; do
    committed="$(source_committed_offset)"
    if [[ "${committed}" == "1" ]] && [[ "$(topic_end_sum "${DLQ_TOPIC}")" -gt "${dlq_before}" ]]; then
        break
    fi
    sleep 0.1
done
[[ "$(source_committed_offset)" == "1" ]] ||
    fail "source offset did not advance after DLQ recovery"
[[ "$(topic_end_sum "${DLQ_TOPIC}")" -gt "${dlq_before}" ]] ||
    fail "invalid record did not reach the recovered DLQ"
echo "PASS: source offset advances only after DLQ delivery recovers"

docker stop --time 8 "${CONSUMER_CONTAINER}" >/dev/null
echo "Access statistics DLQ failure test passed"
