#!/usr/bin/env bash

set -euo pipefail

SERVER_IMAGE="${SHORTLINK_SERVER_IMAGE:-hao-shortlink-server:dev}"
CONSUMER_CONTAINER="${HAOHTTP_STATISTICS_RECOVERY_CONSUMER:-hao-shortlink-statistics-recovery-consumer}"
MYSQL_CONTAINER="${HAOHTTP_MYSQL_CONTAINER:-hao-shortlink-mysql}"
KAFKA_CONTAINER="${HAOHTTP_KAFKA_CONTAINER:-hao-shortlink-kafka}"
NETWORK="${HAOHTTP_KAFKA_NETWORK:-hao-shortlink-dev_default}"
TOPIC="${HAOHTTP_KAFKA_TOPIC:-hao-shortlink.access-events.v1}"
DLQ_TOPIC="${HAOHTTP_KAFKA_DLQ_TOPIC:-hao-shortlink.access-events.dlq.v1}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-statistics-mysql-recovery.XXXXXX")"
CONSUMER_CONFIG="${TMP_DIR}/consumer.conf"
RUN_ID="$(date +%s)-${RANDOM}"
GROUP_ID="hao-shortlink-statistics-mysql-recovery-${RUN_ID}"
CODE="r$(date +%s)${RANDOM}"
EVENT_ID="$(printf '%016x%016x' "$(date +%s)" "${RANDOM}")"

cleanup()
{
    docker rm -f "${CONSUMER_CONTAINER}" >/dev/null 2>&1 || true
    docker start "${MYSQL_CONTAINER}" >/dev/null 2>&1 || true
    rm -rf "${TMP_DIR}"
}

fail()
{
    echo "FAIL: $*" >&2
    docker logs --tail=120 "${CONSUMER_CONTAINER}" >&2 || true
    exit 1
}

wait_for_mysql()
{
    for _ in {1..60}; do
        if [[ "$(docker inspect -f '{{.State.Health.Status}}' "${MYSQL_CONTAINER}" 2>/dev/null || true)" == "healthy" ]]; then
            return 0
        fi
        sleep 1
    done
    fail "MySQL did not become healthy"
}

mysql_value()
{
    docker exec "${MYSQL_CONTAINER}" mysql \
        -N -B -uhao_shortlink -phao_shortlink hao_shortlink \
        -e "$1" 2>/dev/null
}

start_consumer()
{
    docker rm -f "${CONSUMER_CONTAINER}" >/dev/null 2>&1 || true
    docker run -d \
        --name "${CONSUMER_CONTAINER}" \
        --network "${NETWORK}" \
        -v "${CONSUMER_CONFIG}:/tmp/consumer.conf:ro" \
        "${SERVER_IMAGE}" \
        ./shortlink_event_consumer /tmp/consumer.conf \
        >/dev/null

    for _ in {1..40}; do
        local status
        status="$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}" 2>/dev/null || true)"
        if [[ "${status}" == "running" ]]; then
            sleep 1
            if [[ "$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}" 2>/dev/null || true)" == "running" ]]; then
                return 0
            fi
        fi
        if [[ "${status}" == "exited" ]]; then
            fail "statistics consumer exited before starting"
        fi
        sleep 0.1
    done
    fail "statistics consumer did not start"
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker inspect "${MYSQL_CONTAINER}" >/dev/null 2>&1 || fail "MySQL container is missing"
docker inspect "${KAFKA_CONTAINER}" >/dev/null 2>&1 || fail "Kafka container is missing"
docker image inspect "${SERVER_IMAGE}" >/dev/null 2>&1 || fail "server image is missing"
wait_for_mysql

docker exec "${MYSQL_CONTAINER}" mysql \
    -uhao_shortlink -phao_shortlink hao_shortlink \
    -e "INSERT INTO short_links (owner_id, code, original_url, status) SELECT id, '${CODE}', 'https://example.com/mysql-recovery', 'active' FROM users WHERE username_normalized = 'legacy-system'" \
    >/dev/null 2>&1

cat > "${CONSUMER_CONFIG}" <<EOF
consumer.bootstrap_servers=kafka:29092
consumer.topic=${TOPIC}
consumer.group_id=${GROUP_ID}
consumer.client_id=hao-shortlink-statistics-mysql-recovery-${RUN_ID}
consumer.auto_offset_reset=earliest
consumer.poll_timeout_ms=100
consumer.session_timeout_ms=6000
consumer.dlq_topic=${DLQ_TOPIC}
consumer.dlq_client_id=hao-shortlink-statistics-mysql-recovery-dlq-${RUN_ID}
consumer.dlq_message_timeout_ms=2000
consumer.dlq_delivery_timeout_ms=3000
consumer.processing_max_attempts=5
consumer.retry_initial_ms=500
consumer.retry_max_ms=1000
consumer.offset_commit_max_attempts=3
consumer.observability_enabled=true
consumer.observability_listen_address=0.0.0.0
consumer.observability_port=9091
consumer.lag_refresh_ms=100
consumer.kafka_query_timeout_ms=1000
mysql.host=tcp://mysql:3306
mysql.user=hao_shortlink
mysql.password=hao_shortlink
mysql.database=hao_shortlink
EOF

start_consumer
docker stop --time 10 "${MYSQL_CONTAINER}" >/dev/null

payload="{\"schema_version\":1,\"event_type\":\"short_link_access\",\"event_id\":\"${EVENT_ID}\",\"occurred_at_ms\":1784304000999,\"request_id\":\"v19-mysql-recovery\",\"code\":\"${CODE}\",\"result\":\"success\",\"http_status\":302}"
docker exec -i "${KAFKA_CONTAINER}" \
    /opt/kafka/bin/kafka-console-producer.sh \
    --bootstrap-server 127.0.0.1:29092 \
    --topic "${TOPIC}" \
    --reader-property parse.key=true \
    --reader-property key.separator='|' \
    <<< "${CODE}|${payload}"

lag_grew=false
for _ in {1..80}; do
    if [[ "$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}" 2>/dev/null || true)" == "running" ]]; then
        lag_sum="$(docker exec "${CONSUMER_CONTAINER}" curl -fsS \
            http://127.0.0.1:9091/metrics 2>/dev/null | \
            awk '/haohttp_shortlink_access_consumer_lag\{partition=/ {sum += $NF} END {print sum + 0}' || true)"
        [[ "${lag_sum:-0}" -gt 0 ]] && lag_grew=true
    fi
    if [[ "$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}" 2>/dev/null || true)" == "exited" ]]; then
        break
    fi
    sleep 1
done
[[ "$(docker inspect -f '{{.State.Status}}' "${CONSUMER_CONTAINER}")" == "exited" ]] ||
    fail "consumer did not fail after bounded MySQL retries"
[[ "$(docker inspect -f '{{.State.ExitCode}}' "${CONSUMER_CONTAINER}")" != "0" ]] ||
    fail "consumer should exit non-zero after MySQL retry exhaustion"
[[ "${lag_grew}" == "true" ]] || fail "consumer lag did not grow while MySQL blocked offset commit"
docker logs "${CONSUMER_CONTAINER}" 2>&1 | grep -q 'stage=mysql result=failure' ||
    fail "MySQL outage was not observable"
echo "PASS: MySQL outage exhausts bounded retries and leaves the process failed"

docker start "${MYSQL_CONTAINER}" >/dev/null
wait_for_mysql
[[ "$(mysql_value "SELECT COUNT(*) FROM processed_access_events WHERE event_id = '${EVENT_ID}'")" == "0" ]] ||
    fail "failed MySQL attempt should not commit a processing receipt"

start_consumer
for _ in {1..100}; do
    if [[ "$(mysql_value "SELECT COUNT(*) FROM processed_access_events WHERE event_id = '${EVENT_ID}'")" == "1" ]]; then
        break
    fi
    sleep 0.1
done
[[ "$(mysql_value "SELECT COUNT(*) FROM processed_access_events WHERE event_id = '${EVENT_ID}'")" == "1" ]] ||
    fail "event was not processed after MySQL recovery"
[[ "$(mysql_value "SELECT access_count FROM short_link_access_totals t JOIN short_links s ON s.id = t.short_link_id WHERE s.code = '${CODE}' AND t.result = 'success'")" == "1" ]] ||
    fail "recovered event should increment statistics exactly once"
for _ in {1..50}; do
    lag_sum="$(docker exec "${CONSUMER_CONTAINER}" curl -fsS \
        http://127.0.0.1:9091/metrics 2>/dev/null | \
        awk '/haohttp_shortlink_access_consumer_lag\{partition=/ {sum += $NF} END {print sum + 0}' || true)"
    [[ "${lag_sum:-1}" == "0" ]] && break
    sleep 0.1
done
[[ "${lag_sum:-1}" == "0" ]] || fail "consumer lag did not return to zero after MySQL recovery"
echo "PASS: uncommitted event is processed exactly once after MySQL recovery"

docker stop --time 8 "${CONSUMER_CONTAINER}" >/dev/null
echo "Access statistics MySQL recovery test passed"
