#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_API_SMOKE_PORT:-18081}"
BASE_URL="http://127.0.0.1:${PORT}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-smoke.XXXXXX")"
CONFIG_FILE="${TMP_DIR}/server.conf"
SERVER_LOG="${TMP_DIR}/shortlink_server.log"
SERVER_PID=""

cleanup()
{
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

start_server()
{
    stdbuf -oL -eL "${SERVER_BIN}" "${CONFIG_FILE}" > "${SERVER_LOG}" 2>&1 &
    SERVER_PID="$!"

    for _ in {1..50}; do
        if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
            fail "shortlink_server exited before becoming ready"
        fi

        if curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1; then
            return 0
        fi

        sleep 0.1
    done

    fail "health endpoint did not become ready at ${BASE_URL}"
}

fail()
{
    echo "FAIL: $*" >&2
    if [[ -f "${SERVER_LOG}" ]]; then
        echo "---- shortlink_server log ----" >&2
        tail -n 80 "${SERVER_LOG}" >&2 || true
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

expect_not_contains()
{
    local value="$1"
    local unexpected_substring="$2"
    local message="$3"
    if [[ "${value}" == *"${unexpected_substring}"* ]]; then
        fail "${message}: did not expect ${unexpected_substring}"
    fi
}

trap cleanup EXIT

if [[ ! -x "${SERVER_BIN}" ]]; then
    fail "shortlink_server binary not found or not executable at ${SERVER_BIN}; build the project first"
fi

command -v stdbuf >/dev/null 2>&1 || fail "stdbuf is required for stable server log assertions"

cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkSmoke
server.port=${PORT}
server.thread_num=1
storage.type=memory
redis.enabled=false
metrics.enabled=true
EOF

start_server

body_file="${TMP_DIR}/body.txt"
header_file="${TMP_DIR}/headers.txt"

client_request_id="api-smoke-request-01"
status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    -H "X-Request-ID: ${client_request_id}" \
    "${BASE_URL}/api/health")"
body="$(cat "${body_file}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "200" "health status"
expect_contains "${body}" '"status":"ok"' "health body"
expect_contains "${headers}" "X-Request-ID: ${client_request_id}" "health request ID response header"
echo "PASS: GET /api/health"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/api/health/live")"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "liveness status"
expect_contains "${body}" '"status":"ok"' "liveness body"
echo "PASS: GET /api/health/live"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/api/health/ready")"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "readiness status"
expect_contains "${body}" '"status":"ready"' "readiness body"
echo "PASS: GET /api/health/ready"

original_url="https://example.com/api-smoke"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"${original_url}\"}")"
body="$(cat "${body_file}")"
expect_eq "${status}" "201" "create short link status"
expect_contains "${body}" "\"original_url\":\"${original_url}\"" "create short link body"

code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
if [[ -z "${code}" ]]; then
    fail "create short link response did not contain code: ${body}"
fi
expect_contains "${body}" "\"short_url\":\"/s/${code}\"" "create short link short_url"
echo "PASS: POST /api/short-links"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/s/${code}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "302" "redirect status"
expect_contains "${headers}" "Location: ${original_url}" "redirect Location header"
echo "PASS: GET /s/${code}"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/s/notfound")"
body="$(cat "${body_file}")"
expect_eq "${status}" "404" "missing short code status"
expect_contains "${body}" '"code":"short_link_not_found"' "missing short code body"
echo "PASS: GET /s/notfound"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d '{"url":"ftp://example.com/not-supported"}')"
body="$(cat "${body_file}")"
expect_eq "${status}" "400" "invalid URL status"
expect_contains "${body}" '"code":"invalid_url"' "invalid URL body"
echo "PASS: POST /api/short-links rejects invalid URL"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    -H 'X-Request-ID: invalid request id' \
    "${BASE_URL}/api/health")"
headers="$(tr -d '\r' < "${header_file}")"
generated_request_id="$(tr -d '\r' < "${header_file}" | sed -n 's/^X-Request-ID: \([0-9a-f]*\)$/\1/p')"
expect_eq "${status}" "200" "health with invalid request ID status"
if [[ ! "${generated_request_id}" =~ ^[0-9a-f]{32}$ ]]; then
    fail "invalid client request ID should be replaced, got headers: ${headers}"
fi
echo "PASS: invalid X-Request-ID is replaced"

expect_contains "$(cat "${SERVER_LOG}")" "event=http_request" "structured request log event"
expect_contains "$(cat "${SERVER_LOG}")" "route=/s/:code" "structured request log route pattern"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/metrics")"
headers="$(tr -d '\r' < "${header_file}")"
metrics_body="$(cat "${body_file}")"
expect_eq "${status}" "200" "metrics status"
expect_contains "${headers}" "Content-Type: text/plain; version=0.0.4; charset=utf-8" "metrics content type"
expect_contains "${metrics_body}" 'haohttp_http_requests_total{method="GET",route="/api/health",status_class="2xx"}' "health HTTP metric"
expect_contains "${metrics_body}" 'haohttp_http_request_duration_seconds_bucket{method="GET",route="/s/:code"' "redirect latency histogram"
expect_contains "${metrics_body}" 'haohttp_shortlink_create_total{result="success",storage="memory"} 1' "successful create metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_create_total{result="invalid",storage="memory"} 1' "invalid create metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="success",source="memory"} 1' "successful redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="not_found",source="memory"} 1' "missing redirect metric"
expect_not_contains "${metrics_body}" "request_id" "metrics must not expose request IDs"
echo "PASS: GET /metrics"

stop_server
sed -i 's/^metrics.enabled=true$/metrics.enabled=false/' "${CONFIG_FILE}"
start_server
status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/metrics")"
expect_eq "${status}" "404" "disabled metrics endpoint status"
echo "PASS: metrics.enabled=false disables GET /metrics"

echo "API smoke test passed"
