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
COOKIE_JAR="${TMP_DIR}/cookies.txt"
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
auth.registration_enabled=true
auth.session_ttl_seconds=3600
auth.cookie_secure=false
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

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d '{"url":"https://example.com/unauthenticated"}')"
expect_eq "${status}" "401" "unauthenticated create status"
expect_contains "$(cat "${body_file}")" '"code":"authentication_required"' "unauthenticated create body"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/login" \
    -H 'Origin: https://attacker.example' \
    -H 'Content-Type: application/json' \
    -d '{"username":"nobody","password":"invalid-password"}')"
expect_eq "${status}" "403" "cross-origin login status"
expect_contains "$(cat "${body_file}")" '"code":"origin_not_allowed"' "cross-origin login body"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    -c "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/auth/register" \
    -H "Origin: ${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d '{"username":"api_smoke","password":"correct-horse-battery"}')"
body="$(cat "${body_file}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "201" "register status"
expect_contains "${body}" '"username":"api_smoke"' "register user body"
expect_contains "${headers}" 'Set-Cookie: hao_session=' "register session cookie"
expect_contains "${headers}" 'Path=/' "session cookie path"
expect_contains "${headers}" 'HttpOnly' "session cookie HttpOnly"
expect_contains "${headers}" 'SameSite=Lax' "session cookie SameSite"
expect_contains "${headers}" 'Max-Age=' "session cookie fixed lifetime"
session_token="$(awk '$6 == "hao_session" { value=$7 } END { print value }' "${COOKIE_JAR}")"
if [[ ! "${session_token}" =~ ^[0-9a-f]{64}$ ]]; then
    fail "session cookie should contain a 64-character lowercase hex token"
fi

status="$(curl -sS -b "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/api/me")"
expect_eq "${status}" "200" "current user status"
expect_contains "$(cat "${body_file}")" '"username":"api_smoke"' "current user body"
echo "PASS: registration, session cookie and GET /api/me"

status="$(curl -sS -b "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/api/short-links/missing-private-link")"
expect_eq "${status}" "404" "authenticated missing detail status"
expect_not_contains "$(cat "${body_file}")" "${session_token}" \
    "error responses must not expose the session token"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Origin: https://attacker.example' \
    -H 'Content-Type: application/json' \
    -d '{"url":"https://example.com/cross-origin"}')"
expect_eq "${status}" "403" "cross-origin create status"
expect_contains "$(cat "${body_file}")" '"code":"origin_not_allowed"' "cross-origin create body"
echo "PASS: state-changing routes reject cross-origin browser requests"

original_url="https://example.com/api-smoke"
expires_at="$(date -u -d '+10 minutes' '+%Y-%m-%dT%H:%M:%SZ')"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H "Origin: ${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"${original_url}\",\"expires_at\":\"${expires_at}\"}")"
body="$(cat "${body_file}")"
expect_eq "${status}" "201" "create short link status"
expect_contains "${body}" "\"original_url\":\"${original_url}\"" "create short link body"

code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
if [[ -z "${code}" ]]; then
    fail "create short link response did not contain code: ${body}"
fi
expect_contains "${body}" "\"short_url\":\"/s/${code}\"" "create short link short_url"
expect_contains "${body}" "\"expires_at\":\"${expires_at}\"" "create short link expiry"
echo "PASS: POST /api/short-links"

for custom_code in Docs_2026 docs_2026; do
    status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
        -b "${COOKIE_JAR}" \
        -X POST "${BASE_URL}/api/short-links" \
        -H 'Content-Type: application/json' \
        -d "{\"url\":\"https://example.com/${custom_code}\",\"custom_code\":\"${custom_code}\"}")"
    expect_eq "${status}" "201" "custom code ${custom_code} create status"
    expect_contains "$(cat "${body_file}")" "\"code\":\"${custom_code}\"" "custom code response"
done
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d '{"url":"https://example.com/conflict","custom_code":"Docs_2026"}')"
expect_eq "${status}" "409" "duplicate custom code status"
expect_contains "$(cat "${body_file}")" '"code":"short_code_conflict"' "duplicate custom code body"
for invalid_custom_code in health 'bad!'; do
    status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
        -b "${COOKIE_JAR}" \
        -X POST "${BASE_URL}/api/short-links" \
        -H 'Content-Type: application/json' \
        -d "{\"url\":\"https://example.com/invalid-code\",\"custom_code\":\"${invalid_custom_code}\"}")"
    expect_eq "${status}" "400" "invalid custom code ${invalid_custom_code} status"
    expect_contains "$(cat "${body_file}")" '"code":"invalid_custom_code"' "invalid custom code body"
done
echo "PASS: custom code format, reserved word, case sensitivity and conflict"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d '{"url":"https://example.com/already-expired","expires_at":"2000-01-01T00:00:00Z"}')"
body="$(cat "${body_file}")"
expect_eq "${status}" "400" "past create expiry status"
expect_contains "${body}" '"code":"invalid_expires_at"' "past create expiry body"
echo "PASS: POST /api/short-links rejects past expires_at"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d '{"url":"https://example.com/trailing-comma",}')"
body="$(cat "${body_file}")"
expect_eq "${status}" "400" "trailing-comma JSON status"
expect_contains "${body}" '"code":"invalid_request"' "trailing-comma JSON body"
echo "PASS: POST /api/short-links rejects trailing-comma JSON"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/s/${code}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "302" "redirect status"
expect_contains "${headers}" "Location: ${original_url}" "redirect Location header"
echo "PASS: GET /s/${code}"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links/${code}")"
body="$(cat "${body_file}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "200" "owner detail status"
expect_contains "${headers}" 'Cache-Control: no-store' "private management response caching"
expect_contains "${body}" '"status":"active"' "owner detail lifecycle status"
expect_contains "${body}" "\"expires_at\":\"${expires_at}\"" "owner detail expiry"
echo "PASS: GET /api/short-links/{code}"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links?limit=1&status=active")"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "owner list status"
expect_contains "${body}" "\"code\":\"${code}\"" "owner list item"
expect_contains "${body}" '"next_cursor":1' "owner list next cursor"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links?limit=100&cursor=1")"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "internal list second page status"
expect_contains "${body}" '"code":"Docs_2026"' "internal list second page item"
expect_contains "${body}" '"next_cursor":null' "internal list final cursor"
echo "PASS: GET /api/short-links cursor pagination"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X PUT "${BASE_URL}/api/short-links/${code}" \
    -H "Origin: ${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d '{"status":"disabled"}')"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "disable short link status"
expect_contains "${body}" '"status":"disabled"' "disable short link body"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/s/${code}")"
expect_eq "${status}" "404" "disabled short link redirect status"
echo "PASS: disabled short link does not redirect"

past_expires_at="2000-01-01T00:00:00Z"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X PUT "${BASE_URL}/api/short-links/${code}" \
    -H 'Content-Type: application/json' \
    -d "{\"status\":\"active\",\"expires_at\":\"${past_expires_at}\"}")"
expect_eq "${status}" "200" "expire short link status"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/s/${code}")"
expect_eq "${status}" "404" "expired short link redirect status"
echo "PASS: expired short link does not redirect"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X PUT "${BASE_URL}/api/short-links/${code}" \
    -H 'Content-Type: application/json' \
    -d '{"expires_at":null}')"
expect_eq "${status}" "200" "clear short link expiry status"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/s/notfound")"
body="$(cat "${body_file}")"
expect_eq "${status}" "404" "missing short code status"
expect_contains "${body}" '"code":"short_link_not_found"' "missing short code body"
echo "PASS: GET /s/notfound"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
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
expect_contains "${metrics_body}" 'haohttp_shortlink_create_total{result="success",storage="memory"} 3' "successful create metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_create_total{result="invalid",storage="memory"} 6' "invalid create metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="success",source="memory"} 1' "successful redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="not_found",source="memory"} 1' "missing redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="disabled",source="memory"} 1' "disabled redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="expired",source="memory"} 1' "expired redirect metric"
expect_not_contains "${metrics_body}" "request_id" "metrics must not expose request IDs"
expect_not_contains "${metrics_body}" "${session_token}" "metrics must not expose session tokens"
expect_not_contains "$(cat "${SERVER_LOG}")" "correct-horse-battery" "logs must not expose passwords"
expect_not_contains "$(cat "${SERVER_LOG}")" "${session_token}" "logs must not expose session tokens"
echo "PASS: GET /metrics"

status="$(curl -sS -b "${COOKIE_JAR}" -c "${COOKIE_JAR}" -o "${body_file}" \
    -w "%{http_code}" -X DELETE -H "Origin: ${BASE_URL}" "${BASE_URL}/api/auth/session")"
expect_eq "${status}" "204" "logout status"
status="$(curl -sS -b "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/api/me")"
expect_eq "${status}" "401" "revoked session status"
echo "PASS: DELETE /api/auth/session revokes session"

status="$(curl -sS -c "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/login" \
    -H "Origin: ${BASE_URL}" \
    -H 'Content-Type: application/json' \
    -d '{"username":"API_SMOKE","password":"correct-horse-battery"}')"
expect_eq "${status}" "200" "login status"
status="$(curl -sS -b "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/api/me")"
expect_eq "${status}" "200" "current user after login status"
echo "PASS: POST /api/auth/login creates a new session"

stop_server
sed -i 's/^metrics.enabled=true$/metrics.enabled=false/' "${CONFIG_FILE}"
start_server
status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/metrics")"
expect_eq "${status}" "404" "disabled metrics endpoint status"
echo "PASS: metrics.enabled=false disables GET /metrics"

stop_server
sed -i 's/^auth.registration_enabled=true$/auth.registration_enabled=false/' "${CONFIG_FILE}"
start_server
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d '{"username":"registration_disabled","password":"correct-horse-battery"}')"
expect_eq "${status}" "403" "disabled registration status"
expect_contains "$(cat "${body_file}")" '"code":"registration_disabled"' "disabled registration body"
echo "PASS: auth.registration_enabled=false disables registration"

echo "API smoke test passed"
