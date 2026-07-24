#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_REDIS_FALLBACK_PORT:-18083}"
BASE_URL="http://127.0.0.1:${PORT}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
UNAVAILABLE_REDIS_HOST="${HAOHTTP_UNAVAILABLE_REDIS_HOST:-127.0.0.1}"
UNAVAILABLE_REDIS_PORT="${HAOHTTP_UNAVAILABLE_REDIS_PORT:-6390}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-redis-fallback.XXXXXX")"
CONFIG_FILE="${TMP_DIR}/server.conf"
SERVER_LOG="${TMP_DIR}/shortlink_server.log"
SERVER_PID=""
TEST_ORIGINAL_URL="https://example.com/redis-fallback-$(date +%s)-${RANDOM}"
TEST_USERNAME="fallback_${RANDOM}"
COOKIE_JAR="${TMP_DIR}/cookies.txt"

cleanup()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi

    if command -v mysql >/dev/null 2>&1; then
        mysql_cmd -e "DELETE FROM short_links WHERE original_url = '${TEST_ORIGINAL_URL}'" >/dev/null 2>&1 || true
        mysql_cmd -e "DELETE FROM users WHERE username_normalized = '${TEST_USERNAME}'" >/dev/null 2>&1 || true
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

command -v mysql >/dev/null 2>&1 || fail "mysql client is required for Redis fallback test"
command -v redis-cli >/dev/null 2>&1 || fail "redis-cli is required for Redis fallback test"
command -v curl >/dev/null 2>&1 || fail "curl is required for Redis fallback test"

mysql_cmd -e "SELECT 1" >/dev/null 2>&1 ||
    fail "MySQL is not reachable at ${MYSQL_HOST}:${MYSQL_PORT}; start Compose mysql dependency first"
echo "PASS: MySQL dependency reachable"

redis_ping="$(redis-cli -h "${UNAVAILABLE_REDIS_HOST}" -p "${UNAVAILABLE_REDIS_PORT}" PING 2>/dev/null || true)"
if [[ "${redis_ping}" == "PONG" ]]; then
    fail "Redis fallback port ${UNAVAILABLE_REDIS_HOST}:${UNAVAILABLE_REDIS_PORT} is reachable; choose an unavailable port"
fi
echo "PASS: Redis fallback target is unavailable"

cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkRedisFallback
server.port=${PORT}
server.thread_num=1
metrics.enabled=true
auth.registration_enabled=true
auth.session_ttl_seconds=3600
auth.cookie_secure=false
storage.type=mysql
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=2
redis.enabled=true
redis.host=${UNAVAILABLE_REDIS_HOST}
redis.port=${UNAVAILABLE_REDIS_PORT}
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=shortlink:
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

status="$(curl -sS -c "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/register" -H 'Content-Type: application/json' \
    -d "{\"username\":\"${TEST_USERNAME}\",\"password\":\"fallback-password\"}")"
expect_eq "${status}" "201" "register Redis fallback user status"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"${TEST_ORIGINAL_URL}\"}")"
body="$(cat "${body_file}")"
expect_eq "${status}" "201" "create short link status"

code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
if [[ -z "${code}" ]]; then
    fail "create short link response did not contain code: ${body}"
fi
echo "PASS: POST /api/short-links using MySQL storage with Redis unavailable"

mysql_url="$(mysql_cmd -N -B -e "SELECT original_url FROM short_links WHERE code = '${code}' LIMIT 1")"
expect_eq "${mysql_url}" "${TEST_ORIGINAL_URL}" "created short link should be persisted in MySQL"
echo "PASS: MySQL row persisted"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/s/${code}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "302" "redirect should still succeed when Redis is unavailable"
expect_contains "${headers}" "Location: ${TEST_ORIGINAL_URL}" "redirect Location header"
echo "PASS: GET /s/${code} falls back to MySQL when Redis is unavailable"

metrics_body="$(curl -fsS "${BASE_URL}/metrics")"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="success",source="mysql"} 1' "fallback MySQL redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="get",result="error"} 1' "Redis get error metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="set",result="error"} 1' "Redis set error metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_backend_errors_total{backend="redis",operation="get"} 1' "Redis backend error metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_backend_errors_total{backend="redis",operation="set"} 1' "Redis backend set error metric"
echo "PASS: Redis unavailable metrics"

echo "Redis unavailable fallback test passed"
