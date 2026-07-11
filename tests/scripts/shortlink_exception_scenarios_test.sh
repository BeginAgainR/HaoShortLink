#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_EXCEPTION_PORT:-18087}"
BASE_URL="http://127.0.0.1:${PORT}"
REQUESTS="${HAOHTTP_EXCEPTION_REQUESTS:-50}"
CONCURRENCY="${HAOHTTP_EXCEPTION_CONCURRENCY:-8}"
CURL_MAX_TIME_SECONDS="${HAOHTTP_EXCEPTION_CURL_MAX_TIME_SECONDS:-5}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
REDIS_HOST="${HAOHTTP_REDIS_HOST:-docker.orb.internal}"
REDIS_PORT="${HAOHTTP_REDIS_PORT:-16379}"
REDIS_KEY_PREFIX="${HAOHTTP_REDIS_KEY_PREFIX:-shortlink:}"
UNAVAILABLE_REDIS_HOST="${HAOHTTP_UNAVAILABLE_REDIS_HOST:-127.0.0.1}"
UNAVAILABLE_REDIS_PORT="${HAOHTTP_UNAVAILABLE_REDIS_PORT:-6390}"
UNAVAILABLE_MYSQL_HOST="${HAOHTTP_UNAVAILABLE_MYSQL_HOST:-127.0.0.1}"
UNAVAILABLE_MYSQL_PORT="${HAOHTTP_UNAVAILABLE_MYSQL_PORT:-13399}"
ARTIFACT_DIR="${HAOHTTP_EXCEPTION_ARTIFACT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/haohttp-exception-scenarios.XXXXXX")}"
SUMMARY_FILE="${ARTIFACT_DIR}/summary.md"
SERVER_PID=""
RUN_ID="$(date +%s)-${RANDOM}"
URL_PREFIX="https://example.com/exception-${RUN_ID}"
CREATED_ORIGINAL_URL=""
CREATED_CODE=""

cleanup()
{
    stop_server

    if command -v redis-cli >/dev/null 2>&1 && [[ -n "${CREATED_CODE}" ]]; then
        redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY_PREFIX}${CREATED_CODE}" >/dev/null 2>&1 || true
    fi

    if command -v mysql >/dev/null 2>&1; then
        mysql_cmd -e "DELETE FROM short_links WHERE original_url LIKE '${URL_PREFIX}%'" >/dev/null 2>&1 || true
    fi
}

fail()
{
    echo "FAIL: $*" >&2
    if [[ -n "${CURRENT_SERVER_LOG:-}" && -f "${CURRENT_SERVER_LOG}" ]]; then
        echo "---- shortlink_server log ----" >&2
        tail -n 120 "${CURRENT_SERVER_LOG}" >&2 || true
        echo "------------------------------" >&2
    fi
    echo "Artifacts preserved at: ${ARTIFACT_DIR}" >&2
    exit 1
}

usage()
{
    cat <<EOF
Usage: bash tests/scripts/shortlink_exception_scenarios_test.sh

Runs v1.4.4 exception scenario checks against shortlink_server.

Environment variables:
  HAOHTTP_EXCEPTION_REQUESTS              default: ${REQUESTS}
  HAOHTTP_EXCEPTION_CONCURRENCY           default: ${CONCURRENCY}
  HAOHTTP_EXCEPTION_PORT                  default: ${PORT}
  HAOHTTP_EXCEPTION_ARTIFACT_DIR          default: auto-created under /tmp
  HAOHTTP_UNAVAILABLE_REDIS_HOST          default: ${UNAVAILABLE_REDIS_HOST}
  HAOHTTP_UNAVAILABLE_REDIS_PORT          default: ${UNAVAILABLE_REDIS_PORT}
  HAOHTTP_UNAVAILABLE_MYSQL_HOST          default: ${UNAVAILABLE_MYSQL_HOST}
  HAOHTTP_UNAVAILABLE_MYSQL_PORT          default: ${UNAVAILABLE_MYSQL_PORT}
EOF
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

stop_server()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""
}

write_config()
{
    local config_file="$1"
    local mode="$2"
    local redis_enabled="$3"
    local mysql_host="$4"
    local mysql_port="$5"
    local redis_host="$6"
    local redis_port="$7"

    cat > "${config_file}" <<EOF
server.name=HaoShortLinkExceptionScenarios
server.port=${PORT}
server.thread_num=2
storage.type=${mode}
mysql.host=tcp://${mysql_host}:${mysql_port}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=2
redis.enabled=${redis_enabled}
redis.host=${redis_host}
redis.port=${redis_port}
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=${REDIS_KEY_PREFIX}
EOF
}

wait_for_ready()
{
    for _ in {1..80}; do
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            fail "shortlink_server exited before becoming ready"
        fi

        if curl -fsS --max-time 1 "${BASE_URL}/api/health" >/dev/null 2>&1; then
            return 0
        fi

        sleep 0.1
    done

    fail "health endpoint did not become ready at ${BASE_URL}"
}

start_server()
{
    local mode="$1"
    local redis_enabled="$2"
    local mysql_host="${3:-${MYSQL_HOST}}"
    local mysql_port="${4:-${MYSQL_PORT}}"
    local redis_host="${5:-${REDIS_HOST}}"
    local redis_port="${6:-${REDIS_PORT}}"
    local name="$7"
    local server_dir="${ARTIFACT_DIR}/${name}"

    stop_server
    mkdir -p "${server_dir}"
    CURRENT_CONFIG_FILE="${server_dir}/server.conf"
    CURRENT_SERVER_LOG="${server_dir}/shortlink_server.log"

    write_config "${CURRENT_CONFIG_FILE}" "${mode}" "${redis_enabled}" \
        "${mysql_host}" "${mysql_port}" "${redis_host}" "${redis_port}"

    "${SERVER_BIN}" "${CURRENT_CONFIG_FILE}" > "${CURRENT_SERVER_LOG}" 2>&1 < /dev/null &
    SERVER_PID="$!"
    wait_for_ready
}

post_json()
{
    local path="$1"
    local payload="$2"
    local body_file="$3"
    curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -o "${body_file}" -w "%{http_code}" \
        -X POST "${BASE_URL}${path}" \
        -H "Content-Type: application/json" \
        -d "${payload}" 2>/dev/null || printf "000"
}

get_path()
{
    local path="$1"
    local body_file="$2"
    local header_file="$3"
    curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
        "${BASE_URL}${path}" 2>/dev/null || printf "000"
}

run_concurrent_case()
{
    local name="$1"
    local method="$2"
    local path="$3"
    local payload="$4"
    local expected_status="$5"
    local expected_body_substring="$6"
    local case_dir="${ARTIFACT_DIR}/${name}"
    local status_dir="${case_dir}/statuses"
    local bodies_dir="${case_dir}/bodies"
    local status_file="${case_dir}/statuses.txt"
    local failures_file="${case_dir}/failures.txt"
    local worker

    mkdir -p "${status_dir}" "${bodies_dir}"

    worker='
base_url="$1"
method="$2"
path="$3"
payload="$4"
expected_status="$5"
expected_body_substring="$6"
max_time="$7"
bodies_dir="$8"
status_dir="$9"
idx="${10}"
body_file="${bodies_dir}/${idx}.body"
status_file="${status_dir}/${idx}.status"

if [[ "${method}" == "POST" ]]; then
    status="$(curl -sS --max-time "${max_time}" -o "${body_file}" -w "%{http_code}" \
        -X POST "${base_url}${path}" \
        -H "Content-Type: application/json" \
        -d "${payload}" 2>/dev/null || printf "000")"
else
    status="$(curl -sS --max-time "${max_time}" -o "${body_file}" -w "%{http_code}" \
        "${base_url}${path}" 2>/dev/null || printf "000")"
fi

if [[ "${status}" == "${expected_status}" ]] && grep -Fq "${expected_body_substring}" "${body_file}"; then
    printf "%s ok %s\n" "${status}" "${idx}" > "${status_file}"
else
    printf "%s fail %s %s\n" "${status}" "${idx}" "${body_file}" > "${status_file}"
fi
'

    seq 1 "${REQUESTS}" |
        xargs -n 1 -P "${CONCURRENCY}" bash -c "${worker}" bash \
            "${BASE_URL}" "${method}" "${path}" "${payload}" \
            "${expected_status}" "${expected_body_substring}" \
            "${CURL_MAX_TIME_SECONDS}" "${bodies_dir}" "${status_dir}"

    cat "${status_dir}"/*.status | sort -n -k3 > "${status_file}"
    awk '$2 != "ok" { print $0 }' "${status_file}" > "${failures_file}"

    {
        echo "## ${name}"
        echo
        echo "Status distribution:"
        awk '{ count[$1]++ } END { for (code in count) print code, count[code] }' "${status_file}" | sort
        echo
    } >> "${SUMMARY_FILE}"

    if [[ -s "${failures_file}" ]]; then
        cat "${failures_file}" >> "${SUMMARY_FILE}"
        fail "${name} had unexpected responses"
    fi

    echo "PASS: ${name}"
}

check_health()
{
    local status
    local body_file="${ARTIFACT_DIR}/health.body"
    status="$(curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -o "${body_file}" -w "%{http_code}" \
        "${BASE_URL}/api/health" 2>/dev/null || printf "000")"
    expect_eq "${status}" "200" "health status"
}

run_redis_unavailable()
{
    local redis_ping
    redis_ping="$(redis-cli -h "${UNAVAILABLE_REDIS_HOST}" -p "${UNAVAILABLE_REDIS_PORT}" PING 2>/dev/null || true)"
    if [[ "${redis_ping}" == "PONG" ]]; then
        fail "Redis fallback target ${UNAVAILABLE_REDIS_HOST}:${UNAVAILABLE_REDIS_PORT} is reachable; choose an unavailable port"
    fi

    start_server "mysql" "true" "${MYSQL_HOST}" "${MYSQL_PORT}" \
        "${UNAVAILABLE_REDIS_HOST}" "${UNAVAILABLE_REDIS_PORT}" "redis-unavailable"

    local body_file="${ARTIFACT_DIR}/redis-unavailable-create.body"
    local header_file="${ARTIFACT_DIR}/redis-unavailable-redirect.headers"
    CREATED_ORIGINAL_URL="${URL_PREFIX}-redis-unavailable"
    local status
    status="$(post_json "/api/short-links" "{\"url\":\"${CREATED_ORIGINAL_URL}\"}" "${body_file}")"
    expect_eq "${status}" "201" "Redis unavailable create status"
    CREATED_CODE="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
    [[ -n "${CREATED_CODE}" ]] || fail "Redis unavailable create response did not contain code"

    status="$(get_path "/s/${CREATED_CODE}" "${ARTIFACT_DIR}/redis-unavailable-redirect.body" "${header_file}")"
    expect_eq "${status}" "302" "Redis unavailable redirect status"
    expect_contains "$(tr -d '\r' < "${header_file}")" "Location: ${CREATED_ORIGINAL_URL}" "Redis unavailable redirect Location"
    check_health
    {
        echo "## Redis unavailable"
        echo
        echo "- POST /api/short-links: 201"
        echo "- GET /s/{code}: 302"
        echo "- health after fallback: 200"
        echo
    } >> "${SUMMARY_FILE}"
    echo "PASS: Redis unavailable create and redirect fallback"
}

run_mysql_unavailable()
{
    local name="mysql-unavailable"
    local server_dir="${ARTIFACT_DIR}/${name}"
    mkdir -p "${server_dir}"
    CURRENT_CONFIG_FILE="${server_dir}/server.conf"
    CURRENT_SERVER_LOG="${server_dir}/shortlink_server.log"

    stop_server
    write_config "${CURRENT_CONFIG_FILE}" "mysql" "false" \
        "${UNAVAILABLE_MYSQL_HOST}" "${UNAVAILABLE_MYSQL_PORT}" \
        "${REDIS_HOST}" "${REDIS_PORT}"

    "${SERVER_BIN}" "${CURRENT_CONFIG_FILE}" > "${CURRENT_SERVER_LOG}" 2>&1 < /dev/null &
    SERVER_PID="$!"

    for _ in {1..50}; do
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            wait "${SERVER_PID}" 2>/dev/null || true
            SERVER_PID=""
            {
                echo "## MySQL unavailable"
                echo
                echo "- result: server exited before becoming ready"
                echo
            } >> "${SUMMARY_FILE}"
            echo "PASS: MySQL unavailable exits before serving requests"
            return 0
        fi

        if curl -fsS --max-time 1 "${BASE_URL}/api/health" >/dev/null 2>&1; then
            fail "MySQL unavailable server unexpectedly became healthy"
        fi

        sleep 0.1
    done

    fail "MySQL unavailable server did not exit or become healthy within timeout"
}

run_request_validation_for_mode()
{
    local mode="$1"
    local redis_enabled="false"
    if [[ "${mode}" == "mysql-redis" ]]; then
        redis_enabled="true"
        start_server "mysql" "${redis_enabled}" "${MYSQL_HOST}" "${MYSQL_PORT}" "${REDIS_HOST}" "${REDIS_PORT}" "validation-${mode}"
    else
        start_server "${mode}" "${redis_enabled}" "${MYSQL_HOST}" "${MYSQL_PORT}" "${REDIS_HOST}" "${REDIS_PORT}" "validation-${mode}"
    fi

    run_concurrent_case "${mode}-invalid-url" "POST" "/api/short-links" \
        '{"url":"ftp://example.com/not-supported"}' "400" '"code":"invalid_url"'
    run_concurrent_case "${mode}-empty-body" "POST" "/api/short-links" \
        '' "400" '"code":"invalid_request"'
    run_concurrent_case "${mode}-non-json-body" "POST" "/api/short-links" \
        'not-json' "400" '"code":"invalid_request"'
    run_concurrent_case "${mode}-missing-url" "POST" "/api/short-links" \
        '{"name":"missing-url"}' "400" '"code":"invalid_request"'
    run_concurrent_case "${mode}-missing-code" "GET" "/s/notfound-exception-${RUN_ID}" \
        '' "404" '"code":"short_link_not_found"'
    check_health
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ ! -x "${SERVER_BIN}" ]]; then
    fail "shortlink_server binary not found or not executable at ${SERVER_BIN}; build the project first"
fi

command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v mysql >/dev/null 2>&1 || fail "mysql client is required"
command -v redis-cli >/dev/null 2>&1 || fail "redis-cli is required"

mysql_cmd -e "SELECT 1" >/dev/null 2>&1 ||
    fail "MySQL is not reachable at ${MYSQL_HOST}:${MYSQL_PORT}; start Compose mysql dependency first"
redis_ping="$(redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" PING 2>/dev/null || true)"
expect_eq "${redis_ping}" "PONG" "Redis should be reachable at ${REDIS_HOST}:${REDIS_PORT}"

mkdir -p "${ARTIFACT_DIR}"
trap cleanup EXIT

cat > "${SUMMARY_FILE}" <<EOF
# Shortlink exception scenarios

- commit: $(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)
- requests per concurrent case: ${REQUESTS}
- concurrency: ${CONCURRENCY}
- port: ${PORT}
- artifact: ${ARTIFACT_DIR}

EOF

echo "Artifacts: ${ARTIFACT_DIR}"
echo "Summary: ${SUMMARY_FILE}"

run_redis_unavailable
run_mysql_unavailable
run_request_validation_for_mode "memory"
run_request_validation_for_mode "mysql"
run_request_validation_for_mode "mysql-redis"

echo "Shortlink exception scenarios passed"
