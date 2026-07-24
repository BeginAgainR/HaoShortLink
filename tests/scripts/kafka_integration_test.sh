#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
CONSUMER_BIN="${HAOHTTP_CONSUMER_BIN:-${BUILD_DIR}/shortlink_event_consumer}"
KCAT_BIN="${HAOHTTP_KCAT_BIN:-kcat}"
KAFKA_BOOTSTRAP_SERVERS="${HAOHTTP_KAFKA_BOOTSTRAP_SERVERS:-127.0.0.1:19092}"
UNAVAILABLE_BOOTSTRAP_SERVERS="${HAOHTTP_KAFKA_UNAVAILABLE_BOOTSTRAP_SERVERS:-127.0.0.1:1}"
TOPIC="${HAOHTTP_KAFKA_TOPIC:-hao-shortlink.access-events.v1}"
DLQ_TOPIC="${HAOHTTP_KAFKA_DLQ_TOPIC:-hao-shortlink.access-events.dlq.v1}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
PORT="${HAOHTTP_KAFKA_TEST_PORT:-18085}"
BASE_URL="http://127.0.0.1:${PORT}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-kafka-integration.XXXXXX")"
SERVER_CONFIG="${TMP_DIR}/server.conf"
CONSUMER_CONFIG="${TMP_DIR}/consumer.conf"
SERVER_LOG="${TMP_DIR}/shortlink_server.log"
CONSUMER_LOG="${TMP_DIR}/shortlink_event_consumer.log"
RESTART_LOG="${TMP_DIR}/shortlink_event_consumer_restart.log"
TOPIC_DUMP="${TMP_DIR}/topic.txt"
DLQ_DUMP="${TMP_DIR}/dlq.txt"
SERVER_PID=""
CONSUMER_PID=""
RUN_ID="$(date +%s)-${RANDOM}"
GROUP_ID="hao-shortlink-access-event-integration-${RUN_ID}"
AUTH_USERNAME="kafka_${RANDOM}"
OTHER_AUTH_USERNAME="kafka_other_${RANDOM}"
COOKIE_JAR="${TMP_DIR}/cookies.txt"
OTHER_COOKIE_JAR="${TMP_DIR}/other-cookies.txt"

cleanup()
{
    stop_consumer || true
    stop_server
    rm -rf "${TMP_DIR}"
}

stop_server()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""
}

stop_consumer()
{
    if [[ -n "${CONSUMER_PID}" ]] && kill -0 "${CONSUMER_PID}" 2>/dev/null; then
        kill "${CONSUMER_PID}" 2>/dev/null || true
        if ! timeout 8s tail --pid="${CONSUMER_PID}" -f /dev/null >/dev/null 2>&1; then
            kill -KILL "${CONSUMER_PID}" 2>/dev/null || true
            wait "${CONSUMER_PID}" 2>/dev/null || true
            CONSUMER_PID=""
            return 1
        fi
        wait "${CONSUMER_PID}" 2>/dev/null || true
    fi
    CONSUMER_PID=""
}

fail()
{
    echo "FAIL: $*" >&2
    for log_file in "${SERVER_LOG}" "${CONSUMER_LOG}" "${RESTART_LOG}"; do
        if [[ -f "${log_file}" ]]; then
            echo "---- ${log_file} ----" >&2
            tail -n 100 "${log_file}" >&2 || true
        fi
    done
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

expect_contains()
{
    local value="$1"
    local expected="$2"
    local message="$3"
    if [[ "${value}" != *"${expected}"* ]]; then
        fail "${message}: expected to contain ${expected}, got ${value}"
    fi
}

mysql_cmd()
{
    MYSQL_PWD="${MYSQL_PASSWORD}" mysql \
        -h "${MYSQL_HOST}" \
        -P "${MYSQL_PORT}" \
        -u "${MYSQL_USER}" \
        "${MYSQL_DATABASE}" "$@"
}

wait_for_server()
{
    for _ in {1..80}; do
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            fail "shortlink_server exited before becoming healthy"
        fi
        if curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    fail "shortlink_server did not become healthy at ${BASE_URL}"
}

start_server()
{
    : > "${SERVER_LOG}"
    stdbuf -oL -eL "${SERVER_BIN}" "${SERVER_CONFIG}" > "${SERVER_LOG}" 2>&1 &
    SERVER_PID="$!"
    wait_for_server
    establish_session
}

establish_session()
{
    local status
    status="$(curl -sS -c "${COOKIE_JAR}" -o "${TMP_DIR}/auth.body" -w '%{http_code}' \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"${AUTH_USERNAME}\",\"password\":\"kafka-test-password\"}" \
        "${BASE_URL}/api/auth/login")"
    if [[ "${status}" == "401" ]]; then
        status="$(curl -sS -c "${COOKIE_JAR}" -o "${TMP_DIR}/auth.body" -w '%{http_code}' \
            -H 'Content-Type: application/json' \
            -d "{\"username\":\"${AUTH_USERNAME}\",\"password\":\"kafka-test-password\"}" \
            "${BASE_URL}/api/auth/register")"
        expect_eq "${status}" "201" "register Kafka test user"
    else
        expect_eq "${status}" "200" "login Kafka test user"
    fi
}

metric_value()
{
    local metric_line="$1"
    curl -fsS "${BASE_URL}/metrics" | awk -v line="${metric_line}" '$0 ~ line { print $NF; exit }'
}

wait_for_delivery_count()
{
    local expected="$1"
    for _ in {1..80}; do
        local value
        value="$(metric_value 'haohttp_shortlink_access_event_delivery_total\{result="success"\}')"
        if [[ "${value:-0}" -ge "${expected}" ]]; then
            return 0
        fi
        sleep 0.1
    done
    fail "Kafka delivery success metric did not reach ${expected}"
}

record_for_request()
{
    local request_id="$1"
    grep -F "\"request_id\":\"${request_id}\"" "${TOPIC_DUMP}" | tail -n 1 || true
}

trap cleanup EXIT

[[ -x "${SERVER_BIN}" ]] || fail "shortlink_server binary not found at ${SERVER_BIN}"
[[ -x "${CONSUMER_BIN}" ]] || fail "shortlink_event_consumer binary not found at ${CONSUMER_BIN}"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v stdbuf >/dev/null 2>&1 || fail "stdbuf is required"
command -v timeout >/dev/null 2>&1 || fail "timeout is required"
command -v "${KCAT_BIN}" >/dev/null 2>&1 || fail "${KCAT_BIN} is required"
command -v mysql >/dev/null 2>&1 || fail "mysql client is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

"${KCAT_BIN}" -L -b "${KAFKA_BOOTSTRAP_SERVERS}" -t "${TOPIC}" >/dev/null 2>&1 ||
    fail "Kafka topic ${TOPIC} is not reachable at ${KAFKA_BOOTSTRAP_SERVERS}"
echo "PASS: Kafka topic is reachable"

cat > "${SERVER_CONFIG}" <<EOF
server.name=HaoShortLinkKafkaIntegration
server.port=${PORT}
server.thread_num=2
metrics.enabled=true
auth.registration_enabled=true
auth.session_ttl_seconds=3600
auth.cookie_secure=false
storage.type=mysql
statistics.enabled=true
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=2
redis.enabled=false
rate_limit.enabled=false
kafka.enabled=true
kafka.bootstrap_servers=${KAFKA_BOOTSTRAP_SERVERS}
kafka.topic=${TOPIC}
kafka.client_id=hao-shortlink-kafka-integration-${RUN_ID}
kafka.queue_max_messages=1000
kafka.message_timeout_ms=3000
kafka.linger_ms=5
kafka.shutdown_timeout_ms=1000
EOF

start_server

body_file="${TMP_DIR}/body.txt"
status="$(curl -sS -o "${body_file}" -w '%{http_code}' \
    -b "${COOKIE_JAR}" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"https://example.com/kafka-${RUN_ID}\"}" \
    "${BASE_URL}/api/short-links")"
expect_eq "${status}" "201" "create short link"
code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
[[ -n "${code}" ]] || fail "create response did not contain a code"

success_request="v18-${RUN_ID}-success"
disabled_request="v18-${RUN_ID}-disabled"
expired_request="v18-${RUN_ID}-expired"
missing_request="v18-${RUN_ID}-missing"

expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Request-ID: ${success_request}" "${BASE_URL}/s/${code}")" \
    "302" "success redirect while Kafka is healthy"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -X PUT \
    -b "${COOKIE_JAR}" \
    -H 'Content-Type: application/json' -d '{"status":"disabled"}' \
    "${BASE_URL}/api/short-links/${code}")" "200" "disable short link"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Request-ID: ${disabled_request}" "${BASE_URL}/s/${code}")" \
    "404" "disabled redirect while Kafka is healthy"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -X PUT \
    -b "${COOKIE_JAR}" \
    -H 'Content-Type: application/json' \
    -d '{"status":"active","expires_at":"2000-01-01T00:00:00Z"}' \
    "${BASE_URL}/api/short-links/${code}")" "200" "expire short link"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Request-ID: ${expired_request}" "${BASE_URL}/s/${code}")" \
    "404" "expired redirect while Kafka is healthy"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Request-ID: ${missing_request}" "${BASE_URL}/s/missing-${RUN_ID}")" \
    "404" "missing redirect while Kafka is healthy"

wait_for_delivery_count 4
metrics="$(curl -fsS "${BASE_URL}/metrics")"
expect_contains "${metrics}" 'haohttp_shortlink_access_event_enqueue_total{result="accepted"} 4' \
    "accepted producer metric"
expect_contains "${metrics}" 'haohttp_shortlink_access_event_delivery_total{result="success"} 4' \
    "successful delivery metric"
expect_contains "${metrics}" 'haohttp_shortlink_access_event_queue_size 0' "drained producer queue metric"
echo "PASS: producer delivery and metrics"

timeout 15s "${KCAT_BIN}" -C -e -q -b "${KAFKA_BOOTSTRAP_SERVERS}" -t "${TOPIC}" \
    -o beginning -f '%k|%s\n' > "${TOPIC_DUMP}" || fail "failed to read access event topic"

success_record="$(record_for_request "${success_request}")"
disabled_record="$(record_for_request "${disabled_request}")"
expired_record="$(record_for_request "${expired_request}")"
missing_record="$(record_for_request "${missing_request}")"
expect_contains "${success_record}" "${code}|" "success event record key"
expect_contains "${success_record}" '"result":"success","http_status":302' "success event payload"
expect_contains "${disabled_record}" '"result":"disabled","http_status":404' "disabled event payload"
expect_contains "${expired_record}" '"result":"expired","http_status":404' "expired event payload"
expect_contains "${missing_record}" "missing-${RUN_ID}|" "missing event record key"
expect_contains "${missing_record}" '"result":"not_found","http_status":404' "missing event payload"
if [[ "${success_record}" == *"original_url"* || "${success_record}" == *"user_agent"* ]]; then
    fail "access event leaked an excluded field"
fi
echo "PASS: success, disabled, expired and not_found event contracts"

cat > "${CONSUMER_CONFIG}" <<EOF
consumer.bootstrap_servers=${KAFKA_BOOTSTRAP_SERVERS}
consumer.topic=${TOPIC}
consumer.group_id=${GROUP_ID}
consumer.client_id=hao-shortlink-event-consumer-integration-${RUN_ID}
consumer.auto_offset_reset=earliest
consumer.poll_timeout_ms=100
consumer.session_timeout_ms=6000
consumer.dlq_topic=${DLQ_TOPIC}
consumer.dlq_client_id=hao-shortlink-access-statistics-dlq-${RUN_ID}
consumer.dlq_message_timeout_ms=3000
consumer.dlq_delivery_timeout_ms=5000
consumer.processing_max_attempts=3
consumer.retry_initial_ms=100
consumer.retry_max_ms=500
consumer.offset_commit_max_attempts=3
consumer.observability_enabled=true
consumer.observability_listen_address=127.0.0.1
consumer.observability_port=19091
consumer.lag_refresh_ms=200
consumer.kafka_query_timeout_ms=1000
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
EOF

stdbuf -oL -eL "${CONSUMER_BIN}" "${CONSUMER_CONFIG}" > "${CONSUMER_LOG}" 2>&1 &
CONSUMER_PID="$!"
sleep 2

consumer_event_id="$(printf '%016x%016x' "$(date +%s)" "${RANDOM}")"
consumer_payload="{\"schema_version\":1,\"event_type\":\"short_link_access\",\"event_id\":\"${consumer_event_id}\",\"occurred_at_ms\":1784304000999,\"request_id\":\"v19-${RUN_ID}-consumer\",\"code\":\"${code}\",\"result\":\"success\",\"http_status\":302}"
orphan_code="orphan-${RUN_ID}"
orphan_event_id="$(printf '%016x%016x' "$(date +%s)" "${RANDOM}")"
orphan_payload="{\"schema_version\":1,\"event_type\":\"short_link_access\",\"event_id\":\"${orphan_event_id}\",\"occurred_at_ms\":1784304000999,\"request_id\":\"v19-${RUN_ID}-orphan\",\"code\":\"${orphan_code}\",\"result\":\"success\",\"http_status\":302}"
unsupported_schema_payload='{ "schema_version": 2, "event_type": "short_link_access" }'
printf '%s\n' \
    "${code}|${consumer_payload}" \
    "${code}|${consumer_payload}" \
    "wrong-key|${consumer_payload}" \
    "bad-json|not-json" \
    "${orphan_code}|${orphan_payload}" \
    "${code}|${unsupported_schema_payload}" | \
    "${KCAT_BIN}" -P -b "${KAFKA_BOOTSTRAP_SERVERS}" -t "${TOPIC}" -K '|'

for _ in {1..300}; do
    processed_count="$(grep -c "event_id=${consumer_event_id}" "${CONSUMER_LOG}" 2>/dev/null || true)"
    if [[ "${processed_count}" -ge 2 ]] &&
        grep -q 'reason=key_code_mismatch' "${CONSUMER_LOG}" &&
        grep -q 'reason=invalid_json' "${CONSUMER_LOG}" &&
        grep -q 'reason=orphan_short_link' "${CONSUMER_LOG}" &&
        grep -q 'reason=unsupported_schema' "${CONSUMER_LOG}"; then
        break
    fi
    sleep 0.1
done
expect_eq "$(grep -c "event_id=${consumer_event_id}" "${CONSUMER_LOG}" || true)" "2" \
    "v1.9 consumer should persist once and identify the duplicate event"
grep -q 'result=aggregated event_id='"${consumer_event_id}" "${CONSUMER_LOG}" ||
    fail "consumer did not aggregate the first event"
grep -q 'result=duplicate event_id='"${consumer_event_id}" "${CONSUMER_LOG}" ||
    fail "consumer did not identify the duplicate event"
grep -q 'reason=key_code_mismatch' "${CONSUMER_LOG}" || fail "consumer did not DLQ invalid record key"
grep -q 'reason=invalid_json' "${CONSUMER_LOG}" || fail "consumer did not DLQ invalid JSON"
grep -q 'reason=orphan_short_link' "${CONSUMER_LOG}" || fail "consumer did not DLQ orphan event"
grep -q 'reason=unsupported_schema' "${CONSUMER_LOG}" || fail "consumer did not DLQ unsupported schema"

short_link_id="$(mysql_cmd -N -B -e "SELECT id FROM short_links WHERE code = '${code}' LIMIT 1")"
[[ -n "${short_link_id}" ]] || fail "MySQL short-link fixture was not found"
expect_eq "$(mysql_cmd -N -B -e "SELECT access_count FROM short_link_access_totals WHERE short_link_id = ${short_link_id} AND result = 'success'")" \
    "2" "success statistics should include the redirect and one unique injected event"
expect_eq "$(mysql_cmd -N -B -e "SELECT access_count FROM short_link_access_totals WHERE short_link_id = ${short_link_id} AND result = 'disabled'")" \
    "1" "disabled statistics count"
expect_eq "$(mysql_cmd -N -B -e "SELECT access_count FROM short_link_access_totals WHERE short_link_id = ${short_link_id} AND result = 'expired'")" \
    "1" "expired statistics count"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM processed_access_events WHERE event_id = '${consumer_event_id}'")" \
    "1" "duplicate event should have one persistent receipt"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM processed_access_events WHERE event_id = '${orphan_event_id}'")" \
    "0" "orphan event transaction should roll back its receipt"
missing_event_id="$(sed -n 's/.*\"event_id\":\"\([^\"]*\)\".*/\1/p' <<< "${missing_record}")"
[[ -n "${missing_event_id}" ]] || fail "missing event ID could not be extracted"
missing_disposition=""
for _ in {1..200}; do
    missing_disposition="$(mysql_cmd -N -B -e "SELECT disposition FROM processed_access_events WHERE event_id = '${missing_event_id}'")"
    [[ -n "${missing_disposition}" ]] && break
    sleep 0.1
done
expect_eq "${missing_disposition}" \
    "not_found_ignored" "not-found event should keep only an ignored receipt"
echo "PASS: MySQL transaction, aggregate, duplicate and not-found semantics"

statistics_body="$(curl -fsS -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links/${code}/statistics?interval=hour&from=2026-07-17T16:00:00Z&to=2026-07-17T17:00:00Z")"
python3 -c '
import json
import sys

body = json.load(sys.stdin)
expected = {"success": 2, "disabled": 1, "expired": 1, "error": 0}
summary = body.get("summary", {})
points = body.get("trend", {}).get("points", [])
if body.get("consistency") != "eventual":
    raise SystemExit(1)
if summary.get("access_count") != 2 or summary.get("attempt_count") != 4:
    raise SystemExit(1)
if summary.get("result_counts") != expected:
    raise SystemExit(1)
if len(points) != 1:
    raise SystemExit(1)
point = points[0]
if (point.get("bucket_start") != "2026-07-17T16:00:00Z"
        or point.get("access_count") != 1
        or point.get("attempt_count") != 1):
    raise SystemExit(1)
' <<< "${statistics_body}" || fail "hourly statistics response contract: got ${statistics_body}"
day_statistics_body="$(curl -fsS -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links/${code}/statistics?interval=day&from=2026-07-17T00:00:00Z&to=2026-07-18T00:00:00Z")"
python3 -c '
import json
import sys

points = json.load(sys.stdin).get("trend", {}).get("points", [])
if len(points) != 1:
    raise SystemExit(1)
point = points[0]
if (point.get("bucket_start") != "2026-07-17T00:00:00Z"
        or point.get("access_count") != 1
        or point.get("attempt_count") != 1):
    raise SystemExit(1)
' <<< "${day_statistics_body}" || fail "daily statistics response contract: got ${day_statistics_body}"
other_register_status="$(curl -sS -c "${OTHER_COOKIE_JAR}" -o "${body_file}" -w '%{http_code}' \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"${OTHER_AUTH_USERNAME}\",\"password\":\"kafka-test-password\"}" \
    "${BASE_URL}/api/auth/register")"
expect_eq "${other_register_status}" "201" "register second Kafka test user"
expect_eq "$(curl -sS -b "${OTHER_COOKIE_JAR}" -o "${body_file}" -w '%{http_code}' \
    "${BASE_URL}/api/short-links/${code}/statistics")" \
    "404" "cross-owner statistics must not disclose the link"
echo "PASS: statistics query enforces object-level owner authorization"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' \
    -b "${COOKIE_JAR}" "${BASE_URL}/api/short-links/${code}/statistics?interval=week")" \
    "400" "statistics interval validation"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' \
    -b "${COOKIE_JAR}" "${BASE_URL}/api/short-links/${code}/statistics?interval=hour&from=2026-07-17T16:00:01Z&to=2026-07-17T17:00:00Z")" \
    "400" "statistics bucket alignment validation"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' \
    -b "${COOKIE_JAR}" "${BASE_URL}/api/short-links/${code}/statistics?interval=hour&from=2026-01-01T00:00:00Z&to=2026-02-02T00:00:00Z")" \
    "400" "statistics maximum range validation"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' \
    -b "${COOKIE_JAR}" "${BASE_URL}/api/short-links/missing-${RUN_ID}/statistics")" \
    "404" "missing short-link statistics"
expect_contains "$(curl -fsS "${BASE_URL}/metrics")" \
    'haohttp_shortlink_backend_errors_total{backend="mysql",operation="statistics"} 0' \
    "statistics backend metric"
expect_eq "$(curl -sS -o "${body_file}" -w '%{http_code}' \
    -b "${COOKIE_JAR}" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"https://example.com/no-statistics-${RUN_ID}\"}" \
    "${BASE_URL}/api/short-links")" "201" "create short link without access events"
empty_statistics_code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
[[ -n "${empty_statistics_code}" ]] || fail "empty-statistics fixture did not contain a code"
empty_statistics_body="$(curl -fsS -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links/${empty_statistics_code}/statistics")"
expect_contains "${empty_statistics_body}" '"access_count":0,"attempt_count":0' \
    "existing short link with no statistics"
expect_contains "${empty_statistics_body}" '"points":[]' \
    "existing short link should return an empty trend"
echo "PASS: internal statistics API summary, UTC trend and validation boundaries"

expect_contains "$(curl -fsS http://127.0.0.1:19091/health)" '"status":"up"' \
    "consumer health endpoint"
consumer_metrics="$(curl -fsS http://127.0.0.1:19091/metrics)"
for result in aggregated duplicate ignored dlq; do
    value="$(awk -v result="${result}" \
        '$0 ~ "messages_total\\{result=\\\"" result "\\\"\\}" { print $NF; exit }' \
        <<< "${consumer_metrics}")"
    [[ "${value:-0}" -ge 1 ]] || fail "consumer ${result} metric did not advance"
done
last_success="$(awk '/haohttp_shortlink_access_consumer_last_success_unixtime [0-9]+/ { print $NF; exit }' \
    <<< "${consumer_metrics}")"
[[ "${last_success:-0}" -gt 0 ]] || fail "consumer last-success metric did not advance"
if grep -E 'topic=|code=|event_id=|request_id=' <<< "${consumer_metrics}" >/dev/null; then
    fail "consumer metrics exposed a high-cardinality label"
fi
echo "PASS: consumer health and low-cardinality metrics"

timeout 15s "${KCAT_BIN}" -C -e -q -b "${KAFKA_BOOTSTRAP_SERVERS}" -t "${DLQ_TOPIC}" \
    -o beginning -f '%k|%s\n' > "${DLQ_DUMP}" || fail "failed to read access event DLQ"
grep -q '"reason":"key_code_mismatch"' "${DLQ_DUMP}" ||
    fail "DLQ did not contain key/code mismatch"
grep -q '"reason":"invalid_json"' "${DLQ_DUMP}" ||
    fail "DLQ did not contain invalid JSON"
grep -q '"reason":"orphan_short_link"' "${DLQ_DUMP}" ||
    fail "DLQ did not contain orphan short link"
grep -q '"reason":"unsupported_schema"' "${DLQ_DUMP}" ||
    fail "DLQ did not contain unsupported schema"
echo "PASS: permanent invalid events reach the DLQ before source offsets commit"

consumer_shutdown_start="${SECONDS}"
stop_consumer || fail "consumer shutdown exceeded its 8s bound"
consumer_shutdown_elapsed="$(( SECONDS - consumer_shutdown_start ))"

stdbuf -oL -eL "${CONSUMER_BIN}" "${CONSUMER_CONFIG}" > "${RESTART_LOG}" 2>&1 &
CONSUMER_PID="$!"
sleep 3
stop_consumer || fail "restarted consumer shutdown exceeded its 8s bound"
if grep -q 'stage=process result=' "${RESTART_LOG}"; then
    fail "consumer replayed already committed records after restart"
fi
echo "PASS: consumer idempotency, DLQ, manual offset, restart and bounded shutdown=${consumer_shutdown_elapsed}s"

stop_server
cat > "${SERVER_CONFIG}" <<EOF
server.name=HaoShortLinkKafkaInvalidConfig
server.port=${PORT}
server.thread_num=2
metrics.enabled=true
auth.registration_enabled=true
auth.session_ttl_seconds=3600
auth.cookie_secure=false
storage.type=memory
redis.enabled=false
rate_limit.enabled=false
kafka.enabled=true
kafka.bootstrap_servers=${KAFKA_BOOTSTRAP_SERVERS}
kafka.topic=${TOPIC}
kafka.client_id=hao-shortlink-kafka-invalid-${RUN_ID}
kafka.queue_max_messages=1000
kafka.message_timeout_ms=3000
kafka.linger_ms=1000001
kafka.shutdown_timeout_ms=500
EOF
start_server

expect_eq "$(curl -sS -o "${body_file}" -w '%{http_code}' \
    -b "${COOKIE_JAR}" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"https://example.com/invalid-kafka-config-${RUN_ID}\"}" \
    "${BASE_URL}/api/short-links")" "201" "create fixture after Kafka initialization failure"
invalid_config_code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
[[ -n "${invalid_config_code}" ]] || fail "invalid-config fixture response did not contain a code"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' "${BASE_URL}/s/${invalid_config_code}")" \
    "302" "Kafka initialization failure redirect"
grep -q 'event=kafka_access_event_producer status=disabled fail_open=true' "${SERVER_LOG}" ||
    fail "invalid Kafka producer config did not fall back to Noop publisher"
echo "PASS: producer initialization failure falls back to Noop without changing HTTP semantics"

stop_server
cat > "${SERVER_CONFIG}" <<EOF
server.name=HaoShortLinkKafkaFailOpen
server.port=${PORT}
server.thread_num=2
metrics.enabled=true
auth.registration_enabled=true
auth.session_ttl_seconds=3600
auth.cookie_secure=false
storage.type=memory
redis.enabled=false
rate_limit.enabled=false
kafka.enabled=true
kafka.bootstrap_servers=${UNAVAILABLE_BOOTSTRAP_SERVERS}
kafka.topic=${TOPIC}
kafka.client_id=hao-shortlink-kafka-fail-open-${RUN_ID}
kafka.queue_max_messages=1
kafka.message_timeout_ms=2000
kafka.linger_ms=0
kafka.shutdown_timeout_ms=500
EOF
start_server

expect_eq "$(curl -sS -o "${body_file}" -w '%{http_code}' \
    -b "${COOKIE_JAR}" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"https://example.com/fail-open-${RUN_ID}\"}" \
    "${BASE_URL}/api/short-links")" "201" "create fail-open fixture"
fail_open_code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
[[ -n "${fail_open_code}" ]] || fail "fail-open create response did not contain a code"

for request in {1..30}; do
    expect_eq "$(curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
        "${BASE_URL}/s/${fail_open_code}")" "302" "Kafka unavailable redirect ${request}"
done
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' "${BASE_URL}/api/health/ready")" \
    "200" "Kafka unavailable readiness"

queue_full="$(metric_value 'haohttp_shortlink_access_event_enqueue_total\{result="queue_full"\}')"
if [[ "${queue_full:-0}" -lt 1 ]]; then
    fail "bounded producer queue did not report queue_full"
fi
sleep 3
delivery_failure="$(metric_value 'haohttp_shortlink_access_event_delivery_total\{result="failure"\}')"
if [[ "${delivery_failure:-0}" -lt 1 ]]; then
    fail "unavailable broker did not report delivery failure"
fi

shutdown_start="${SECONDS}"
stop_server
shutdown_elapsed="$(( SECONDS - shutdown_start ))"
if [[ "${shutdown_elapsed}" -gt 4 ]]; then
    fail "producer shutdown exceeded bound: ${shutdown_elapsed}s"
fi
grep -q 'event=server_shutdown' "${SERVER_LOG}" ||
    fail "shortlink_server did not execute graceful signal shutdown"
grep -q 'event=kafka_access_event_producer stage=shutdown' "${SERVER_LOG}" ||
    fail "Kafka producer did not execute bounded shutdown flush"
echo "PASS: Kafka unavailable and queue full remain fail-open; shutdown=${shutdown_elapsed}s"

echo "Kafka integration test passed"
