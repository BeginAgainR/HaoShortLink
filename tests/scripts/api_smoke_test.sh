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
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${TMP_DIR}"
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

trap cleanup EXIT

if [[ ! -x "${SERVER_BIN}" ]]; then
    fail "shortlink_server binary not found or not executable at ${SERVER_BIN}; build the project first"
fi

cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkSmoke
server.port=${PORT}
server.thread_num=1
storage.type=memory
redis.enabled=false
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

status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/api/health")"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "health status"
expect_contains "${body}" '"status":"ok"' "health body"
echo "PASS: GET /api/health"

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

echo "API smoke test passed"
