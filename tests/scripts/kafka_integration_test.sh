#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
CONSUMER_BIN="${HAOHTTP_CONSUMER_BIN:-${BUILD_DIR}/shortlink_event_consumer}"
KCAT_BIN="${HAOHTTP_KCAT_BIN:-kcat}"
KAFKA_BOOTSTRAP_SERVERS="${HAOHTTP_KAFKA_BOOTSTRAP_SERVERS:-127.0.0.1:19092}"
UNAVAILABLE_BOOTSTRAP_SERVERS="${HAOHTTP_KAFKA_UNAVAILABLE_BOOTSTRAP_SERVERS:-127.0.0.1:1}"
TOPIC="${HAOHTTP_KAFKA_TOPIC:-hao-shortlink.access-events.v1}"
PORT="${HAOHTTP_KAFKA_TEST_PORT:-18085}"
BASE_URL="http://127.0.0.1:${PORT}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-kafka-integration.XXXXXX")"
SERVER_CONFIG="${TMP_DIR}/server.conf"
CONSUMER_CONFIG="${TMP_DIR}/consumer.conf"
SERVER_LOG="${TMP_DIR}/shortlink_server.log"
CONSUMER_LOG="${TMP_DIR}/shortlink_event_consumer.log"
RESTART_LOG="${TMP_DIR}/shortlink_event_consumer_restart.log"
TOPIC_DUMP="${TMP_DIR}/topic.txt"
SERVER_PID=""
CONSUMER_PID=""
RUN_ID="$(date +%s)-${RANDOM}"
GROUP_ID="hao-shortlink-access-event-integration-${RUN_ID}"

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

"${KCAT_BIN}" -L -b "${KAFKA_BOOTSTRAP_SERVERS}" -t "${TOPIC}" >/dev/null 2>&1 ||
    fail "Kafka topic ${TOPIC} is not reachable at ${KAFKA_BOOTSTRAP_SERVERS}"
echo "PASS: Kafka topic is reachable"

cat > "${SERVER_CONFIG}" <<EOF
server.name=HaoShortLinkKafkaIntegration
server.port=${PORT}
server.thread_num=2
metrics.enabled=true
storage.type=memory
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
    -H 'Content-Type: application/json' -d '{"status":"disabled"}' \
    "${BASE_URL}/internal/short-links/${code}")" "200" "disable short link"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Request-ID: ${disabled_request}" "${BASE_URL}/s/${code}")" \
    "404" "disabled redirect while Kafka is healthy"
expect_eq "$(curl -sS -o /dev/null -w '%{http_code}' -X PUT \
    -H 'Content-Type: application/json' \
    -d '{"status":"active","expires_at":"2000-01-01T00:00:00Z"}' \
    "${BASE_URL}/internal/short-links/${code}")" "200" "expire short link"
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
EOF

stdbuf -oL -eL "${CONSUMER_BIN}" "${CONSUMER_CONFIG}" > "${CONSUMER_LOG}" 2>&1 &
CONSUMER_PID="$!"
sleep 2

consumer_event_id="$(printf '%016x%016x' "$(date +%s)" "${RANDOM}")"
consumer_payload="{\"schema_version\":1,\"event_type\":\"short_link_access\",\"event_id\":\"${consumer_event_id}\",\"occurred_at_ms\":1784304000999,\"request_id\":\"v18-${RUN_ID}-consumer\",\"code\":\"consumer-${RUN_ID}\",\"result\":\"success\",\"http_status\":302}"
printf '%s\n' \
    "consumer-${RUN_ID}|${consumer_payload}" \
    "consumer-${RUN_ID}|${consumer_payload}" \
    "wrong-key|${consumer_payload}" | \
    "${KCAT_BIN}" -P -b "${KAFKA_BOOTSTRAP_SERVERS}" -t "${TOPIC}" -K '|'

for _ in {1..100}; do
    processed_count="$(grep -c "event_id=${consumer_event_id}" "${CONSUMER_LOG}" 2>/dev/null || true)"
    if [[ "${processed_count}" -ge 2 ]] && grep -q 'reason=key_code_mismatch' "${CONSUMER_LOG}"; then
        break
    fi
    sleep 0.1
done
expect_eq "$(grep -c "event_id=${consumer_event_id}" "${CONSUMER_LOG}" || true)" "2" \
    "v1.8 consumer should accept duplicate valid events without fake in-memory dedupe"
grep -q 'reason=key_code_mismatch' "${CONSUMER_LOG}" || fail "consumer did not discard invalid record key"
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
echo "PASS: consumer duplicate boundary, discard, manual offset, restart and bounded shutdown=${consumer_shutdown_elapsed}s"

stop_server
cat > "${SERVER_CONFIG}" <<EOF
server.name=HaoShortLinkKafkaInvalidConfig
server.port=${PORT}
server.thread_num=2
metrics.enabled=true
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
