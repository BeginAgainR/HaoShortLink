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
RUN_ID="$(date +%s)-${RANDOM}"
TEST_URL_PREFIX="https://example.com/integration-${RUN_ID}"
TEST_ORIGINAL_URL="${TEST_URL_PREFIX}-primary"
TEST_USERNAME="integration_${RANDOM}"
OTHER_USERNAME="other_${RANDOM}"
CONCURRENT_USERNAME="raceuser_${RANDOM}"
CONCURRENT_CUSTOM_CODE="Race${RANDOM}"
COOKIE_JAR="${TMP_DIR}/cookies.txt"
OTHER_COOKIE_JAR="${TMP_DIR}/other-cookies.txt"
MIGRATION_TABLE="short_links_v17_migration_test"

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
        mysql_cmd -e "DELETE FROM short_links WHERE original_url LIKE '${TEST_URL_PREFIX}%'" >/dev/null 2>&1 || true
        mysql_cmd -e "DELETE FROM users WHERE username_normalized = '${TEST_USERNAME}'" >/dev/null 2>&1 || true
        mysql_cmd -e "DELETE FROM users WHERE username_normalized = '${OTHER_USERNAME}'" >/dev/null 2>&1 || true
        mysql_cmd -e "DELETE FROM users WHERE username_normalized = '${CONCURRENT_USERNAME}'" >/dev/null 2>&1 || true
        mysql_cmd -e "DROP TABLE IF EXISTS ${MIGRATION_TABLE}" >/dev/null 2>&1 || true
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
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM schema_migrations WHERE version = 5")" "1" "v2.0 migration should be recorded"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '${MYSQL_DATABASE}' AND TABLE_NAME = 'short_links' AND COLUMN_NAME = 'owner_id'")" "1" "short_links owner column should exist"

status_column_count="$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '${MYSQL_DATABASE}' AND TABLE_NAME = 'short_links' AND COLUMN_NAME = 'status'")"
if [[ "${status_column_count}" == "0" ]]; then
    mysql_cmd < "${ROOT_DIR}/apps/shortlink_server/sql/003_add_short_link_lifecycle.sql"
fi
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '${MYSQL_DATABASE}' AND TABLE_NAME = 'short_links' AND COLUMN_NAME IN ('status', 'expires_at')")" "2" "lifecycle columns should exist"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA = '${MYSQL_DATABASE}' AND TABLE_NAME = 'short_links' AND CONSTRAINT_TYPE = 'CHECK'")" "1" "short_links lifecycle status CHECK should exist"

mysql_cmd -e "DROP TABLE IF EXISTS ${MIGRATION_TABLE}; CREATE TABLE ${MIGRATION_TABLE} (id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, code VARCHAR(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin DEFAULT NULL, original_url TEXT NOT NULL, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY (id), UNIQUE KEY uk_v17_migration_code (code)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4; INSERT INTO ${MIGRATION_TABLE} (code, original_url) VALUES ('legacy01', 'https://example.com/legacy');"
sed "s/ALTER TABLE short_links/ALTER TABLE ${MIGRATION_TABLE}/" \
    "${ROOT_DIR}/apps/shortlink_server/sql/003_add_short_link_lifecycle.sql" | mysql_cmd
legacy_lifecycle="$(mysql_cmd -N -B -e "SELECT CONCAT(status, ':', IF(expires_at IS NULL, 'null', 'set')) FROM ${MIGRATION_TABLE} WHERE code = 'legacy01'")"
expect_eq "${legacy_lifecycle}" "active:null" "v1.7 migration should preserve legacy rows as active and non-expiring"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS WHERE CONSTRAINT_SCHEMA = '${MYSQL_DATABASE}' AND TABLE_NAME = '${MIGRATION_TABLE}' AND CONSTRAINT_TYPE = 'CHECK'")" "1" "migration test table lifecycle status CHECK should exist"
if mysql_cmd -e "UPDATE ${MIGRATION_TABLE} SET status = 'invalid' WHERE code = 'legacy01'" \
    >/dev/null 2>&1; then
    fail "lifecycle status CHECK should reject invalid status"
fi
expect_eq "$(mysql_cmd -N -B -e "SELECT status FROM ${MIGRATION_TABLE} WHERE code = 'legacy01'")" "active" "rejected invalid status should leave legacy row unchanged"
mysql_cmd -e "DROP TABLE ${MIGRATION_TABLE}"
echo "PASS: v1.7 legacy table migration"

redis_ping="$(redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" PING 2>/dev/null || true)"
expect_eq "${redis_ping}" "PONG" "Redis should be reachable at ${REDIS_HOST}:${REDIS_PORT}"
echo "PASS: Redis dependency reachable"

cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkIntegration
server.port=${PORT}
server.thread_num=4
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
redis.host=${REDIS_HOST}
redis.port=${REDIS_PORT}
redis.database=0
redis.ttl_seconds=0
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

status="$(curl -sS -c "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"${TEST_USERNAME}\",\"password\":\"integration-password\"}")"
expect_eq "${status}" "201" "register integration user status"
password_hash="$(mysql_cmd -N -B -e "SELECT password_hash FROM users WHERE username_normalized = '${TEST_USERNAME}'")"
expect_contains "${password_hash}" 'scrypt$32768$8$1$' "password should use the configured scrypt format"
if [[ "${password_hash}" == *"integration-password"* ]]; then
    fail "password hash must not contain the plaintext password"
fi
raw_session_token="$(awk '$6 == "hao_session" { value=$7 } END { print value }' "${COOKIE_JAR}")"
[[ "${raw_session_token}" =~ ^[0-9a-f]{64}$ ]] || fail "session cookie should be 64 lowercase hex characters"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM user_sessions WHERE token_hash = '${raw_session_token}'")" \
    "0" "database must not store the raw session token"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM user_sessions WHERE token_hash = SHA2('${raw_session_token}', 256)")" \
    "1" "database should store the SHA-256 session token digest"
echo "PASS: MySQL user registration and session"

username_race_pids=()
for attempt in 1 2; do
    (
        curl -sS -o "${TMP_DIR}/username-race-${attempt}.body" -w "%{http_code}" \
            -X POST "${BASE_URL}/api/auth/register" \
            -H 'Content-Type: application/json' \
            -d "{\"username\":\"${CONCURRENT_USERNAME}\",\"password\":\"integration-password\"}" \
            > "${TMP_DIR}/username-race-${attempt}.status"
    ) &
    username_race_pids+=("$!")
done
for pid in "${username_race_pids[@]}"; do
    wait "${pid}"
done
username_race_statuses="$(sort "${TMP_DIR}"/username-race-*.status | tr '\n' ' ' | sed 's/ $//')"
expect_eq "${username_race_statuses}" "201 409" "concurrent duplicate username status distribution"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM users WHERE username_normalized = '${CONCURRENT_USERNAME}'")" \
    "1" "concurrent duplicate username should create one user"
echo "PASS: username uniqueness is enforced under concurrent registration"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
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

custom_race_pids=()
for attempt in 1 2; do
    (
        curl -sS -o "${TMP_DIR}/custom-race-${attempt}.body" -w "%{http_code}" \
            -b "${COOKIE_JAR}" \
            -X POST "${BASE_URL}/api/short-links" \
            -H 'Content-Type: application/json' \
            -d "{\"url\":\"${TEST_URL_PREFIX}-custom-${attempt}\",\"custom_code\":\"${CONCURRENT_CUSTOM_CODE}\"}" \
            > "${TMP_DIR}/custom-race-${attempt}.status"
    ) &
    custom_race_pids+=("$!")
done
for pid in "${custom_race_pids[@]}"; do
    wait "${pid}"
done
custom_race_statuses="$(sort "${TMP_DIR}"/custom-race-*.status | tr '\n' ' ' | sed 's/ $//')"
expect_eq "${custom_race_statuses}" "201 409" "concurrent duplicate custom code status distribution"
expect_eq "$(mysql_cmd -N -B -e "SELECT COUNT(*) FROM short_links WHERE code = '${CONCURRENT_CUSTOM_CODE}'")" \
    "1" "concurrent duplicate custom code should create one link"
lowercase_custom_code="$(printf '%s' "${CONCURRENT_CUSTOM_CODE}" | tr '[:upper:]' '[:lower:]')"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"${TEST_URL_PREFIX}-custom-lowercase\",\"custom_code\":\"${lowercase_custom_code}\"}")"
expect_eq "${status}" "201" "case-distinct custom code status"
echo "PASS: custom code uniqueness is concurrent and case-sensitive"

mysql_url="$(mysql_cmd -N -B -e "SELECT original_url FROM short_links WHERE code = '${TEST_CODE}' LIMIT 1")"
expect_eq "${mysql_url}" "${TEST_ORIGINAL_URL}" "created short link should be persisted in MySQL"
expect_eq "$(mysql_cmd -N -B -e "SELECT u.username_normalized FROM short_links l JOIN users u ON u.id = l.owner_id WHERE l.code = '${TEST_CODE}'")" "${TEST_USERNAME}" "created link should belong to authenticated user"
echo "PASS: MySQL row persisted"

status="$(curl -sS -c "${OTHER_COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"${OTHER_USERNAME}\",\"password\":\"integration-password\"}")"
expect_eq "${status}" "201" "register second integration user status"
status="$(curl -sS -b "${OTHER_COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"${TEST_URL_PREFIX}-cross-owner-conflict\",\"custom_code\":\"${CONCURRENT_CUSTOM_CODE}\"}")"
expect_eq "${status}" "409" "cross-owner custom code conflict status"
status="$(curl -sS -b "${OTHER_COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/api/short-links/${TEST_CODE}")"
expect_eq "${status}" "404" "cross-owner detail status"
status="$(curl -sS -b "${OTHER_COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    -X PUT "${BASE_URL}/api/short-links/${TEST_CODE}" \
    -H 'Content-Type: application/json' -d '{"status":"disabled"}')"
expect_eq "${status}" "404" "cross-owner update status"
expect_not_owner_body="$(curl -fsS -b "${OTHER_COOKIE_JAR}" "${BASE_URL}/api/short-links")"
if [[ "${expect_not_owner_body}" == *"${TEST_CODE}"* ]]; then
    fail "cross-owner list exposed ${TEST_CODE}"
fi
echo "PASS: object-level owner authorization"

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
expect_contains "${redis_after}" "v2|" "redirect should backfill versioned Redis cache"
expect_contains "${redis_after}" "|active|" "versioned Redis cache should include lifecycle status"
expect_contains "${redis_after}" "|${TEST_ORIGINAL_URL}" "versioned Redis cache should include original URL"
initial_cache_ttl="$(redis-cli --raw -h "${REDIS_HOST}" -p "${REDIS_PORT}" TTL "${REDIS_KEY_PREFIX}${TEST_CODE}")"
if (( initial_cache_ttl < 1 || initial_cache_ttl > 3600 )); then
    fail "invalid zero cache TTL should fall back to bounded default, got ${initial_cache_ttl}"
fi
echo "PASS: Redis cache backfilled"

status="$(curl -sS -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
    "${BASE_URL}/s/${TEST_CODE}")"
headers="$(tr -d '\r' < "${header_file}")"
expect_eq "${status}" "302" "second redirect status"
expect_contains "${headers}" "Location: ${TEST_ORIGINAL_URL}" "second redirect Location header"
echo "PASS: GET /s/${TEST_CODE} redirects from Redis cache"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links/${TEST_CODE}")"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "internal MySQL detail status"
expect_contains "${body}" '"status":"active"' "internal MySQL detail lifecycle status"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    "${BASE_URL}/api/short-links?limit=1&status=active")"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "internal MySQL list status"
expect_contains "${body}" '"items":[' "internal MySQL list body"
echo "PASS: authenticated lifecycle detail and list using MySQL"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X PUT "${BASE_URL}/api/short-links/${TEST_CODE}" \
    -H 'Content-Type: application/json' \
    -d '{"status":"disabled"}')"
body="$(cat "${body_file}")"
expect_eq "${status}" "200" "disable MySQL short link status"
expect_contains "${body}" '"status":"disabled"' "disable MySQL short link body"
expect_eq "$(redis-cli --raw -h "${REDIS_HOST}" -p "${REDIS_PORT}" EXISTS "${REDIS_KEY_PREFIX}${TEST_CODE}")" "0" "disable should invalidate hot cache"

redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" SETEX \
    "${REDIS_KEY_PREFIX}${TEST_CODE}" 3600 "${TEST_ORIGINAL_URL}" >/dev/null
status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/s/${TEST_CODE}")"
expect_eq "${status}" "404" "legacy stale cache must not bypass disabled status"
stale_replacement="$(redis-cli --raw -h "${REDIS_HOST}" -p "${REDIS_PORT}" GET "${REDIS_KEY_PREFIX}${TEST_CODE}" 2>/dev/null || true)"
expect_contains "${stale_replacement}" "v2|" "legacy cache should be replaced with lifecycle-aware record"
expect_contains "${stale_replacement}" "|disabled|" "replacement cache should preserve disabled status"
echo "PASS: disabled link and legacy stale cache handling"

status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X PUT "${BASE_URL}/api/short-links/${TEST_CODE}" \
    -H 'Content-Type: application/json' \
    -d '{"status":"active","expires_at":"2000-01-01T00:00:00Z"}')"
expect_eq "${status}" "200" "expire MySQL short link status"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" "${BASE_URL}/s/${TEST_CODE}")"
expect_eq "${status}" "404" "expired MySQL short link must not redirect"
expect_eq "$(redis-cli --raw -h "${REDIS_HOST}" -p "${REDIS_PORT}" EXISTS "${REDIS_KEY_PREFIX}${TEST_CODE}")" "0" "expired record should not remain cached"

future_expires_at="$(date -u -d '+10 minutes' '+%Y-%m-%dT%H:%M:%SZ')"
status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -b "${COOKIE_JAR}" \
    -X PUT "${BASE_URL}/api/short-links/${TEST_CODE}" \
    -H 'Content-Type: application/json' \
    -d "{\"expires_at\":\"${future_expires_at}\"}")"
expect_eq "${status}" "200" "restore future expiry status"
status="$(curl -sS -o /dev/null -w "%{http_code}" "${BASE_URL}/s/${TEST_CODE}")"
expect_eq "${status}" "302" "future-expiring MySQL short link should redirect"
cache_ttl="$(redis-cli --raw -h "${REDIS_HOST}" -p "${REDIS_PORT}" TTL "${REDIS_KEY_PREFIX}${TEST_CODE}")"
if (( cache_ttl < 1 || cache_ttl > 600 )); then
    fail "business expiry should cap Redis TTL, got ${cache_ttl}"
fi
echo "PASS: expired and future-expiring lifecycle semantics"

metrics_body="$(curl -fsS "${BASE_URL}/metrics")"
expect_contains "${metrics_body}" 'haohttp_shortlink_create_total{result="success",storage="mysql"} ' "MySQL create metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="success",source="mysql"} ' "MySQL redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="success",source="redis"} ' "Redis redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="get",result="miss"} ' "Redis miss metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="get",result="hit"} ' "Redis hit metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_cache_operations_total{operation="set",result="success"} ' "Redis set metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="disabled",source="mysql"} ' "disabled redirect metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_redirect_total{result="expired",source="mysql"} ' "expired redirect metric"
echo "PASS: MySQL / Redis metrics"

kill "${SERVER_PID}"
wait "${SERVER_PID}" 2>/dev/null || true
SERVER_PID=""
"${SERVER_BIN}" "${CONFIG_FILE}" > "${SERVER_LOG}" 2>&1 &
SERVER_PID="$!"
for _ in {1..50}; do
    if curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
expect_eq "$(curl -sS -b "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" "${BASE_URL}/api/me")" \
    "200" "persistent session after server restart"
echo "PASS: MySQL session remains valid across server restart"

mysql_cmd -e "UPDATE user_sessions SET expires_at = '2000-01-01 00:00:00' WHERE token_hash = SHA2('${raw_session_token}', 256)"
expect_eq "$(curl -sS -b "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" "${BASE_URL}/api/me")" \
    "401" "expired session status"
status="$(curl -sS -c "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/login" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"${TEST_USERNAME}\",\"password\":\"integration-password\"}")"
expect_eq "${status}" "200" "login after session expiry status"
echo "PASS: absolute session expiry invalidates the old cookie"

mysql_cmd -e "UPDATE users SET status = 'disabled' WHERE username_normalized = '${TEST_USERNAME}'"
expect_eq "$(curl -sS -b "${COOKIE_JAR}" -o "${body_file}" -w "%{http_code}" "${BASE_URL}/api/me")" \
    "401" "disabled user session status"
disabled_login_status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/login" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"${TEST_USERNAME}\",\"password\":\"integration-password\"}")"
disabled_login_body="$(cat "${body_file}")"
missing_login_status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/auth/login" \
    -H 'Content-Type: application/json' \
    -d '{"username":"missing_integration_user","password":"integration-password"}')"
missing_login_body="$(cat "${body_file}")"
expect_eq "${disabled_login_status}" "401" "disabled user login status"
expect_eq "${missing_login_status}" "401" "missing user login status"
expect_eq "${disabled_login_body}" "${missing_login_body}" "login failure should not disclose account status"
mysql_cmd -e "UPDATE users SET status = 'active' WHERE username_normalized = '${TEST_USERNAME}'"
echo "PASS: disabling a user invalidates sessions without login enumeration"

echo "MySQL / Redis integration test passed"
