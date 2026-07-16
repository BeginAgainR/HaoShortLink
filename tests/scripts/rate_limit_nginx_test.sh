#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BASE_URL="${HAOHTTP_NGINX_BASE_URL:-http://127.0.0.1:8080}"
DIRECT_URL="${HAOHTTP_DIRECT_BASE_URL:-http://127.0.0.1:18080}"
RUN_ID="$(date +%s)-${RANDOM}"
KEY_PREFIX="rate-limit:nginx-test:${RUN_ID}:"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-nginx-rate-limit.XXXXXX")"
CONFIG_FILE="${TMP_DIR}/server.conf"
OVERRIDE_FILE="${TMP_DIR}/compose.override.yaml"
BODY_FILE="${TMP_DIR}/body.txt"
HEADER_FILE="${TMP_DIR}/headers.txt"

fail()
{
    echo "FAIL: $*" >&2
    docker compose -f compose.yaml -f "${OVERRIDE_FILE}" logs --tail=100 shortlink_server nginx >&2 || true
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

wait_for_ready()
{
    for _ in {1..60}; do
        if curl -fsS "${BASE_URL}/api/health/ready" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    fail "Nginx rate limit target did not become ready"
}

cleanup()
{
    docker compose exec -T redis redis-cli DEL "${KEY_PREFIX}global" >/dev/null 2>&1 || true
    docker compose exec -T mysql mysql \
        -uhao_shortlink -phao_shortlink hao_shortlink \
        -e "DELETE FROM short_links WHERE original_url LIKE 'https://example.com/nginx-rate-limit-${RUN_ID}-%'" \
        >/dev/null 2>&1 || true
    docker compose up -d --no-build --force-recreate shortlink_server nginx >/dev/null 2>&1 || true
    rm -rf "${TMP_DIR}"
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"

cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkNginxRateLimitTest
server.port=8080
server.thread_num=4
metrics.enabled=true
storage.type=mysql
mysql.host=tcp://mysql:3306
mysql.user=hao_shortlink
mysql.password=hao_shortlink
mysql.database=hao_shortlink
mysql.pool_size=4
redis.enabled=true
redis.host=redis
redis.port=6379
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=shortlink:
rate_limit.enabled=true
rate_limit.requests=2
rate_limit.window_seconds=60
rate_limit.key_prefix=${KEY_PREFIX}
EOF

cat > "${OVERRIDE_FILE}" <<EOF
services:
  shortlink_server:
    volumes:
      - ${CONFIG_FILE}:/opt/hao-shortlink/apps/shortlink_server/config/server.container.conf.example:ro
EOF

cd "${ROOT_DIR}"
docker compose -f compose.yaml -f "${OVERRIDE_FILE}" up -d --no-build --force-recreate \
    mysql redis shortlink_server nginx
wait_for_ready

for suffix in one two; do
    status="$(curl -sS -o "${BODY_FILE}" -w "%{http_code}" \
        -X POST "${BASE_URL}/api/short-links" \
        -H 'Content-Type: application/json' \
        -d "{\"url\":\"https://example.com/nginx-rate-limit-${RUN_ID}-${suffix}\"}")"
    expect_eq "${status}" "201" "Nginx request ${suffix} inside allowance"
done

internal_code="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${BODY_FILE}")"
if [[ -z "${internal_code}" ]]; then
    fail "create response did not contain code for internal boundary check"
fi
expect_eq "$(curl -sS -o /dev/null -w "%{http_code}" "${BASE_URL}/internal/short-links/${internal_code}")" "404" \
    "Nginx should block internal lifecycle API"
expect_eq "$(curl -sS -o /dev/null -w "%{http_code}" "${DIRECT_URL}/internal/short-links/${internal_code}")" "200" \
    "localhost direct port should expose internal lifecycle API"
echo "PASS: Nginx blocks internal lifecycle API while localhost direct access works"

status="$(curl -sS -D "${HEADER_FILE}" -o "${BODY_FILE}" -w "%{http_code}" \
    -X POST "${BASE_URL}/api/short-links" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"https://example.com/nginx-rate-limit-${RUN_ID}-limited\"}")"
body="$(cat "${BODY_FILE}")"
headers="$(tr -d '\r' < "${HEADER_FILE}")"
expect_eq "${status}" "429" "Nginx request above allowance"
expect_contains "${headers}" "Retry-After: " "Nginx should preserve Retry-After"
expect_contains "${body}" '"code":"rate_limit_exceeded"' "Nginx 429 body"

metrics_body="$(curl -fsS "${DIRECT_URL}/metrics")"
expect_contains "${metrics_body}" 'haohttp_shortlink_rate_limit_checks_total{result="allowed"} 2' "Nginx allowed metric"
expect_contains "${metrics_body}" 'haohttp_shortlink_rate_limit_checks_total{result="limited"} 1' "Nginx limited metric"

expect_eq "$(curl -sS -o /dev/null -w "%{http_code}" "${BASE_URL}/api/health/live")" "200" \
    "Nginx liveness"
expect_eq "$(curl -sS -o /dev/null -w "%{http_code}" "${BASE_URL}/api/health/ready")" "200" \
    "Nginx readiness"

echo "Nginx rate limit smoke test passed"
