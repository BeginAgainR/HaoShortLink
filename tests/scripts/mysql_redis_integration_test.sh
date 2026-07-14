#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_INTEGRATION_PORT:-18082}"
BASE_URL="http://127.0.0.1:${PORT}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
REDIS_HOST="${HAOHTTP_REDIS_HOST:-docker.orb.internal}"
REDIS_PORT="${HAOHTTP_REDIS_PORT:-16379}"
REDIS_KEY_PREFIX="${HAOHTTP_REDIS_KEY_PREFIX:-shortlink:}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-integration.XXXXXX")"
CONFIG_FILE="${TMP_DIR}/server.conf"
SERVER_LOG="${TMP_DIR}/shortlink_server.log"
SERVER_PID=""
TEST_CODE=""
TEST_ORIGINAL_URL="https://example.com/integration-$(date +%s)-${RANDOM}"

cleanup()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi

    if command -v redis-cli >/dev/null 2>&1 && [[ -n "${TEST_CODE}" ]]; then
        redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY_PREFIX}${TEST_CODE}" >/dev/null 2>&1 || true
    fi

    if command -v mysql >/dev/null 2>&1; then
        mysql_cmd -e "DELETE FROM short_links WHERE original_url = '${TEST_ORIGINAL_URL}'" >/dev/null 2>&1 || true
    fi

    rm -rf "${TMP_DIR}"
}

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

trap cleanup EXIT

if [[ ! -x "${SERVER_BIN}" ]]; then
    fail "shortlink_server binary not found or not executable at ${SERVER_BIN}; build the project first"
fi

command -v mysql >/dev/null 2>&1 || fail "mysql client is required for integration test"
command -v redis-cli >/dev/null 2>&1 || fail "redis-cli is required for integration test"
command -v curl >/dev/null 2>&1 || fail "curl is required for integration test"

mysql_cmd -e "SELECT 1" >/dev/null 2>&1 ||
    fail "MySQL is not reachable at ${MYSQL_HOST}:${MYSQL_PORT}; start Compose mysql dependency first"
echo "PASS: MySQL dependency reachable"

table_count="$(mysql_cmd -N -B -e "SHOW TABLES LIKE 'short_links'" | wc -l | tr -d ' ')"
expect_eq "${table_count}" "1" "short_links table should exist"

code_collation="$(mysql_cmd -N -B -e "SELECT COLLATION_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '${MYSQL_DATABASE}' AND TABLE_NAME = 'short_links' AND COLUMN_NAME = 'code'")"
expect_eq "${code_collation}" "utf8mb4_bin" "short_links.code should use case-sensitive collation"

redis_ping="$(redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" PING 2>/dev/null || true)"
expect_eq "${redis_ping}" "PONG" "Redis should be reachable at ${REDIS_HOST}:${REDIS_PORT}"
echo "PASS: Redis dependency reachable"

cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkIntegration
server.port=${PORT}
server.thread_num=1
metrics.enabled=true
storage.type=mysql
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=2
redis.enabled=true
redis.host=${REDIS_HOST}
redis.port=${REDIS_PORT}
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=${REDIS_KEY_PREFIX}
EOF

"${SERVER_BIN}" "${CONFIG_FILE}" > "${SERVER_LOG}" 2>&1 &
SERVER_PID="$!"

for _ in {1..50}; do
    if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
        fail "shortlink_server exited before becoming ready"
    fi

    if curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1; then
        break
    fi

    sleep 0.1
done

if ! curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1; then
    fail "health endpoint did not become ready at ${BASE_URL}"
fi

body_file="${TMP_DIR}/body.txt"
header_file="${TMP_DIR}/headers.txt"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"${TEST_ORIGINAL_URL}\"}")"
body="$(cat "${body_file}")"
expect_eq "${status}" "201" "create short link status"
expect_contains "${body}" "\"original_url\":\"${TEST_ORIGINAL_URL}\"" "create short link body"

TEST_CODE="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
if [[ -z "${TEST_CODE}" ]]; then
    fail "create short link response did not contain code: ${body}"
fi
echo "PASS: POST /api/short-links using MySQL storage"

mysql_url="$(mysql_cmd -N -B -e "SELECT original_url FROM short_links WHERE code = '${TEST_CODE}' LIMIT 1")"
expect_eq "${mysql_url}" "${TEST_ORIGINAL_URL}" "created short link should be persisted in MySQL"
echo "PASS: MySQL row persisted"

redis_before="$(redis-cli --raw -h "${REDIS_HOST}" -p "${REDIS_PORT}" GET "${REDIS_KEY_PREFIX}${TEST_CODE}" 2>/dev/null || true)"
if [[ "${redis_before}" == "${TEST_ORIGINAL_URL}" ]]; then
    fail "Redis cache should not contain the new code before the first redirect"
fi

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/s/${TEST_CODE}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "302" "redirect status"
expect_contains "${headers}" "Location: ${TEST_ORIGINAL_URL}" "redirect Location header"
echo "PASS: GET /s/${TEST_CODE} redirects from MySQL source"

redis_after="$(redis-cli --raw -h "${REDIS_HOST}" -p "${REDIS_PORT}" GET "${REDIS_KEY_PREFIX}${TEST_CODE}" 2>/dev/null || true)"
expect_eq "${redis_after}" "${TEST_ORIGINAL_URL}" "redirect should backfill Redis cache"
echo "PASS: Redis cache backfilled"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/s/${TEST_CODE}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "302" "second redirect status"
expect_contains "${headers}" "Location: ${TEST_ORIGINAL_URL}" "second redirect Location header"
echo "PASS: GET /s/${TEST_CODE} redirects from Redis cache"

metrics_body="$(curl -fsS "${BASE_URL}/metrics")"
expect_contains "${metrics_body}" 'haohttp_shortlink_create_total{result="success",storage="mysql"} 1' "MySQL create metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="success",source="mysql"} 1' "MySQL redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="success",source="redis"} 1' "Redis redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="get",result="miss"} 1' "Redis miss metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="get",result="hit"} 1' "Redis hit metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="set",result="success"} 1' "Redis set metric"
echo "PASS: MySQL / Redis metrics"

echo "MySQL / Redis integration test passed"
