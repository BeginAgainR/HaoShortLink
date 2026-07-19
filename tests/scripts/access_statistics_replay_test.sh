#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SERVER_IMAGE="${SHORTLINK_SERVER_IMAGE:-hao-shortlink-server:dev}"
CONSUMER_CONTAINER="${HAOHTTP_STATISTICS_REPLAY_CONSUMER:-hao-shortlink-statistics-replay-consumer}"
MYSQL_CONTAINER="${HAOHTTP_MYSQL_CONTAINER:-hao-shortlink-mysql}"
KAFKA_CONTAINER="${HAOHTTP_KAFKA_CONTAINER:-hao-shortlink-kafka}"
NETWORK="${HAOHTTP_KAFKA_NETWORK:-hao-shortlink-dev_default}"
DLQ_TOPIC="${HAOHTTP_KAFKA_DLQ_TOPIC:-hao-shortlink.access-events.dlq.v1}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-statistics-replay.XXXXXX")"
PRIMARY_CONFIG="${TMP_DIR}/primary.conf"
REBUILD_CONFIG="${TMP_DIR}/rebuild.conf"
RUN_ID="$(date +%s)-${RANDOM}"
SOURCE_TOPIC="hao-shortlink.access-events.replay-${RUN_ID}"
PRIMARY_GROUP="hao-shortlink-statistics-replay-${RUN_ID}"
REBUILD_GROUP="hao-shortlink-statistics-rebuild-${RUN_ID}"
REBUILD_DATABASE="hao_shortlink_rebuild_$(date +%s)_${RANDOM}"
CODE="p$(date +%s)${RANDOM}"
EVENT_ONE="$(printf '%016x%016x' "$(date +%s)" "${RANDOM}")"
EVENT_TWO="$(printf '%016x%016x' "$(date +%s)" "${RANDOM}")"
REBUILD_CREATED=false

fail()
{
    echo "FAIL: $*" >&2
    docker logs --tail=120 "${CONSUMER_CONTAINER}" >&2 || true
    exit 1
}

cleanup()
{
    docker rm -f "${CONSUMER_CONTAINER}" >/dev/null 2>&1 || true
    docker exec "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-topics.sh \
        --bootstrap-server 127.0.0.1:29092 \
        --delete --topic "${SOURCE_TOPIC}" >/dev/null 2>&1 || true
    if [[ "${REBUILD_CREATED}" == "true" && "${REBUILD_DATABASE}" == hao_shortlink_rebuild_* ]]; then
        docker exec "${MYSQL_CONTAINER}" mysql \
            -uroot -phao_shortlink_root \
            -e "DROP DATABASE IF EXISTS \`${REBUILD_DATABASE}\`" \
            >/dev/null 2>&1 || true
    fi
    rm -rf "${TMP_DIR}"
}

write_config()
{
    local path="$1"
    local group="$2"
    local database="$3"
    cat > "${path}" <<EOF
consumer.bootstrap_servers=kafka:29092
consumer.topic=${SOURCE_TOPIC}
consumer.group_id=${group}
consumer.client_id=${group}
consumer.auto_offset_reset=earliest
consumer.poll_timeout_ms=100
consumer.session_timeout_ms=6000
consumer.dlq_topic=${DLQ_TOPIC}
consumer.dlq_client_id=${group}-dlq
consumer.dlq_message_timeout_ms=2000
consumer.dlq_delivery_timeout_ms=3000
consumer.processing_max_attempts=3
consumer.retry_initial_ms=100
consumer.retry_max_ms=500
consumer.offset_commit_max_attempts=3
consumer.observability_enabled=true
consumer.observability_listen_address=0.0.0.0
consumer.observability_port=9091
consumer.lag_refresh_ms=5000
consumer.kafka_query_timeout_ms=1000
mysql.host=tcp://mysql:3306
mysql.user=hao_shortlink
mysql.password=hao_shortlink
mysql.database=${database}
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
    for _ in {1..80}; do
        local status
        status="$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}" 2>/dev/null || true)"
        if [[ "${status}" == "running" ]] &&
            docker exec "${CONSUMER_CONTAINER}" curl -fsS \
                http://127.0.0.1:9091/health >/dev/null 2>&1; then
            return 0
        fi
        [[ "${status}" != "exited" ]] || fail "replay consumer exited during startup"
        sleep 0.1
    done
    fail "replay consumer did not become healthy"
}

stop_consumer()
{
    docker stop --time 8 "${CONSUMER_CONTAINER}" >/dev/null
}

mysql_value()
{
    local database="$1"
    local query="$2"
    docker exec "${MYSQL_CONTAINER}" mysql \
        -N -B -uhao_shortlink -phao_shortlink "${database}" \
        -e "${query}" 2>/dev/null
}

wait_for_receipts()
{
    local database="$1"
    for _ in {1..200}; do
        if [[ "$(mysql_value "${database}" "SELECT COUNT(*) FROM processed_access_events WHERE event_id IN ('${EVENT_ONE}', '${EVENT_TWO}')")" == "2" ]]; then
            return 0
        fi
        sleep 0.1
    done
    fail "database ${database} did not receive both replay fixtures"
}

totals_signature()
{
    local database="$1"
    mysql_value "${database}" \
        "SELECT GROUP_CONCAT(CONCAT(t.result, ':', t.access_count) ORDER BY t.result SEPARATOR ',') FROM short_link_access_totals t JOIN short_links s ON s.id = t.short_link_id WHERE s.code = '${CODE}'"
}

hourly_signature()
{
    local database="$1"
    mysql_value "${database}" \
        "SELECT GROUP_CONCAT(CONCAT(h.bucket_start_epoch, ':', h.result, ':', h.access_count) ORDER BY h.bucket_start_epoch, h.result SEPARATOR ',') FROM short_link_access_hourly h JOIN short_links s ON s.id = h.short_link_id WHERE s.code = '${CODE}'"
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker inspect "${MYSQL_CONTAINER}" >/dev/null 2>&1 || fail "MySQL container is missing"
docker inspect "${KAFKA_CONTAINER}" >/dev/null 2>&1 || fail "Kafka container is missing"
docker image inspect "${SERVER_IMAGE}" >/dev/null 2>&1 || fail "server image is missing"

docker exec "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-topics.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --create --topic "${SOURCE_TOPIC}" \
    --partitions 1 --replication-factor 1 \
    --config cleanup.policy=delete --config retention.ms=3600000 \
    >/dev/null
docker exec "${MYSQL_CONTAINER}" mysql \
    -uhao_shortlink -phao_shortlink hao_shortlink \
    -e "INSERT INTO short_links (code, original_url, status) VALUES ('${CODE}', 'https://example.com/replay', 'active')" \
    >/dev/null 2>&1

write_config "${PRIMARY_CONFIG}" "${PRIMARY_GROUP}" "hao_shortlink"
payload_one="{\"schema_version\":1,\"event_type\":\"short_link_access\",\"event_id\":\"${EVENT_ONE}\",\"occurred_at_ms\":1784304000999,\"request_id\":\"v19-replay-one\",\"code\":\"${CODE}\",\"result\":\"success\",\"http_status\":302}"
payload_two="{\"schema_version\":1,\"event_type\":\"short_link_access\",\"event_id\":\"${EVENT_TWO}\",\"occurred_at_ms\":1784307600999,\"request_id\":\"v19-replay-two\",\"code\":\"${CODE}\",\"result\":\"disabled\",\"http_status\":404}"
printf '%s\n' "${CODE}|${payload_one}" "${CODE}|${payload_two}" | \
    docker exec -i "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-console-producer.sh \
        --bootstrap-server 127.0.0.1:29092 \
        --topic "${SOURCE_TOPIC}" \
        --reader-property parse.key=true \
        --reader-property key.separator='|'

start_consumer "${PRIMARY_CONFIG}"
wait_for_receipts "hao_shortlink"
stop_consumer
primary_totals="$(totals_signature "hao_shortlink")"
primary_hourly="$(hourly_signature "hao_shortlink")"
[[ "${primary_totals}" == "disabled:1,success:1" ]] ||
    fail "unexpected primary projection before replay: ${primary_totals}"

docker exec "${KAFKA_CONTAINER}" /opt/kafka/bin/kafka-consumer-groups.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --group "${PRIMARY_GROUP}" \
    --topic "${SOURCE_TOPIC}" \
    --reset-offsets --to-earliest --execute >/dev/null
start_consumer "${PRIMARY_CONFIG}"
for _ in {1..200}; do
    duplicate_count="$(docker exec "${CONSUMER_CONTAINER}" curl -fsS \
        http://127.0.0.1:9091/metrics 2>/dev/null | \
        awk '/messages_total\{result="duplicate"\}/ {print $NF; exit}' || true)"
    [[ "${duplicate_count:-0}" -ge 2 ]] && break
    sleep 0.1
done
[[ "${duplicate_count:-0}" -ge 2 ]] || fail "controlled replay did not expose duplicate events"
stop_consumer
[[ "$(totals_signature "hao_shortlink")" == "${primary_totals}" ]] ||
    fail "idempotent replay changed cumulative statistics"
[[ "$(hourly_signature "hao_shortlink")" == "${primary_hourly}" ]] ||
    fail "idempotent replay changed hourly statistics"
echo "PASS: controlled offset replay is idempotent and observable as duplicate"

docker exec "${MYSQL_CONTAINER}" mysql -uroot -phao_shortlink_root \
    -e "CREATE DATABASE \`${REBUILD_DATABASE}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci; GRANT ALL PRIVILEGES ON \`${REBUILD_DATABASE}\`.* TO 'hao_shortlink'@'%';" \
    >/dev/null 2>&1
REBUILD_CREATED=true
for migration in "${ROOT_DIR}"/apps/shortlink_server/sql/*.sql; do
    docker exec -i "${MYSQL_CONTAINER}" mysql \
        -uroot -phao_shortlink_root "${REBUILD_DATABASE}" \
        < "${migration}"
done
docker exec "${MYSQL_CONTAINER}" mysql \
    -uhao_shortlink -phao_shortlink "${REBUILD_DATABASE}" \
    -e "INSERT INTO short_links (code, original_url, status) VALUES ('${CODE}', 'https://example.com/replay', 'active')" \
    >/dev/null 2>&1
write_config "${REBUILD_CONFIG}" "${REBUILD_GROUP}" "${REBUILD_DATABASE}"
start_consumer "${REBUILD_CONFIG}"
wait_for_receipts "${REBUILD_DATABASE}"
stop_consumer
[[ "$(totals_signature "${REBUILD_DATABASE}")" == "${primary_totals}" ]] ||
    fail "isolated rebuild totals differ from the primary projection"
[[ "$(hourly_signature "${REBUILD_DATABASE}")" == "${primary_hourly}" ]] ||
    fail "isolated rebuild hourly data differs from the primary projection"
echo "PASS: new group rebuilds the retained range into an isolated database"

echo "Access statistics replay and rebuild test passed"
