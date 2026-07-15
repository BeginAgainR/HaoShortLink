#!/usr/bin/env bash

set -euo pipefail

BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_RATE_LIMIT_PORT:-18087}"
BASE_URL="http://127.0.0.1:${PORT}"
REDIS_HOST="${HAOHTTP_REDIS_HOST:-docker.orb.internal}"
REDIS_PORT="${HAOHTTP_REDIS_PORT:-16379}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
RUN_ID="$(date +%s)-${RANDOM}"
KEY_PREFIX="rate-limit:test:${RUN_ID}:"
REDIS_KEY="${KEY_PREFIX}global"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-rate-limit.XXXXXX")"
CONFIG_FILE="${TMP_DIR}/server.conf"
SERVER_LOG="${TMP_DIR}/shortlink_server.log"
SERVER_PID=""

fail()
{
    echo "FAIL: $*" >&2
    if [[ -f "${SERVER_LOG}" ]]; then
        echo "---- shortlink_server log ----" >&2
        tail -n 100 "${SERVER_LOG}" >&2 || true
        echo "------------------------------" >&2
    fi
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
    local expected_substring="$2"
    local message="$3"
    if [[ "${value}" != *"${expected_substring}"* ]]; then
        fail "${message}: expected to contain ${expected_substring}, got ${value}"
    fi
}

stop_server()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""
}

cleanup()
{
    stop_server
    redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY}" >/dev/null 2>&1 || true
    if command -v mysql >/dev/null 2>&1; then
        mysql_cmd -e "DELETE FROM short_links WHERE original_url LIKE 'https://example.com/rate-limit-${RUN_ID}-%'" \
            >/dev/null 2>&1 || true
    fi
    rm -rf "${TMP_DIR}"
}

mysql_cmd()
{
    mysql \
        -h "${MYSQL_HOST}" \
        -P "${MYSQL_PORT}" \
        -u "${MYSQL_USER}" \
        -p"${MYSQL_PASSWORD}" \
        "${MYSQL_DATABASE}" \
        "$@"
}

write_config()
{
    local redis_port="$1"
    cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkRateLimitTest
server.port=${PORT}
server.thread_num=4
metrics.enabled=true
storage.type=memory
redis.enabled=false
redis.host=${REDIS_HOST}
redis.port=${redis_port}
redis.database=0
rate_limit.enabled=true
rate_limit.requests=3
rate_limit.window_seconds=2
rate_limit.key_prefix=${KEY_PREFIX}
EOF
}

write_mysql_config()
{
    cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkRateLimitMySqlTest
server.port=${PORT}
server.thread_num=2
metrics.enabled=true
storage.type=mysql
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=2
redis.enabled=false
redis.host=${REDIS_HOST}
redis.port=${REDIS_PORT}
redis.database=0
rate_limit.enabled=true
rate_limit.requests=3
rate_limit.window_seconds=2
rate_limit.key_prefix=${KEY_PREFIX}
EOF
}

start_server()
{
    : > "${SERVER_LOG}"
    stdbuf -oL -eL "${SERVER_BIN}" "${CONFIG_FILE}" > "${SERVER_LOG}" 2>&1 &
    SERVER_PID="$!"

    for _ in {1..50}; do
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            fail "shortlink_server exited before becoming ready"
        fi
        if curl -fsS "${BASE_URL}/api/health/ready" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done

    fail "rate limit test server did not become ready"
}

create_request()
{
    local suffix="$1"
    local output_file="$2"
    curl -sS -o "${output_file}" -w "%{http_code}" \
        -X POST "${BASE_URL}/api/short-links" \
        -H 'Content-Type: application/json' \
        -d "{\"url\":\"https://example.com/rate-limit-${RUN_ID}-${suffix}\"}"
}

trap cleanup EXIT

[[ -x "${SERVER_BIN}" ]] || fail "shortlink_server binary not found at ${SERVER_BIN}"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v redis-cli >/dev/null 2>&1 || fail "redis-cli is required"
command -v mysql >/dev/null 2>&1 || fail "mysql client is required"
command -v stdbuf >/dev/null 2>&1 || fail "stdbuf is required for stable log assertions"

redis_ping="$(redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" PING 2>/dev/null || true)"
expect_eq "${redis_ping}" "PONG" "Redis dependency should be reachable"

body_file="${TMP_DIR}/body.txt"
header_file="${TMP_DIR}/headers.txt"

write_config "${REDIS_PORT}"
start_server

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" -d '{}')"
expect_eq "${status}" "400" "invalid request inside allowance should keep validation response"

status="$(create_request one "${body_file}")"
expect_eq "${status}" "201" "first valid request inside allowance"
status="$(create_request two "${body_file}")"
expect_eq "${status}" "201" "second valid request inside allowance"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"https://example.com/rate-limit-${RUN_ID}-limited\"}")"
body="$(cat "${body_file}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "429" "request above allowance"
expect_contains "${body}" '"code":"rate_limit_exceeded"' "429 error body"
expect_contains "${headers}" "Retry-After: " "429 Retry-After header"
retry_after="$(tr -d '\r' < "${header_file}" | sed -n 's/^Retry-After: \([0-9][0-9]*\)$/\1/p')"
[[ "${retry_after}" =~ ^[1-9][0-9]*$ ]] || fail "Retry-After should be a positive integer"
echo "PASS: fixed-window allowance and 429 response"

sleep 3
status="$(create_request after-window "${body_file}")"
expect_eq "${status}" "201" "request after window expiry"
echo "PASS: fixed-window allowance recovers after expiry"

redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY}" >/dev/null
request_pids=()
for i in $(seq 1 20); do
    (
        create_request "concurrent-${i}" "${TMP_DIR}/concurrent-${i}.body" \
            > "${TMP_DIR}/concurrent-${i}.status"
    ) &
    request_pids+=("$!")
done
for request_pid in "${request_pids[@]}"; do
    wait "${request_pid}"
done

created_count="$(grep -h -c '^201$' "${TMP_DIR}"/concurrent-*.status | awk '{ total += $1 } END { print total + 0 }')"
limited_count="$(grep -h -c '^429$' "${TMP_DIR}"/concurrent-*.status | awk '{ total += $1 } END { print total + 0 }')"
expect_eq "${created_count}" "3" "concurrent allowed count"
expect_eq "${limited_count}" "17" "concurrent limited count"
echo "PASS: concurrent Redis Lua checks preserve the exact allowance"

metrics_body="$(curl -fsS "${BASE_URL}/metrics")"
expect_contains "${metrics_body}" 'haohttp_shortlink_rate_limit_checks_total{result="allowed"} 7' "allowed metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_rate_limit_checks_total{result="limited"} 18' "limited metric"

stop_server
write_config "1"
start_server

status="$(create_request fail-open "${body_file}")"
expect_eq "${status}" "201" "Redis unavailable should fail open"
metrics_body="$(curl -fsS "${BASE_URL}/metrics")"
expect_contains "${metrics_body}" 'haohttp_shortlink_rate_limit_checks_total{result="error"} 1' "fail-open rate limit metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_backend_errors_total{backend="redis",operation="rate_limit"} 1' "fail-open backend metric"
expect_contains "$(cat "${SERVER_LOG}")" 'rate_limit result=error fail_open=true' "fail-open log"
echo "PASS: Redis unavailable fails open with logs and metrics"

stop_server
redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY}" >/dev/null
mysql_cmd -e "SELECT 1" >/dev/null 2>&1 || fail "MySQL dependency should be reachable"
write_mysql_config
start_server

status="$(create_request mysql-cache-disabled "${body_file}")"
expect_eq "${status}" "201" "MySQL create with query cache disabled and rate limit enabled"
mysql_count="$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM short_links WHERE original_url = 'https://example.com/rate-limit-${RUN_ID}-mysql-cache-disabled'")"
expect_eq "${mysql_count}" "1" "rate-limited create should persist through MySQL repository"
echo "PASS: MySQL storage can use Redis rate limiting with query cache disabled"

echo "Rate limit protection test passed"
