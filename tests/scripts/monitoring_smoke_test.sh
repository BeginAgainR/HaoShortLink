#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NGINX_URL="${HAOHTTP_NGINX_URL:-http://127.0.0.1:8080}"
PROMETHEUS_URL="${HAOHTTP_PROMETHEUS_URL:-http://127.0.0.1:9090}"
GRAFANA_URL="${HAOHTTP_GRAFANA_URL:-http://127.0.0.1:3000}"
GRAFANA_ADMIN_USER="${GRAFANA_ADMIN_USER:-admin}"
GRAFANA_ADMIN_PASSWORD="${GRAFANA_ADMIN_PASSWORD:-admin}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-monitoring.XXXXXX")"
COOKIE_JAR="${TMP_DIR}/cookies.txt"
RUN_ID="$(date +%s)-${RANDOM}"
TEST_USERNAME="monitor_${RANDOM}"
TEST_ORIGINAL_URL="https://example.com/monitoring-${RUN_ID}"

cleanup()
{
    docker compose exec -T mysql mysql \
        -uhao_shortlink -phao_shortlink hao_shortlink \
        -e "DELETE FROM short_links WHERE original_url = '${TEST_ORIGINAL_URL}'" \
        >/dev/null 2>&1 || true
    docker compose exec -T mysql mysql \
        -uhao_shortlink -phao_shortlink hao_shortlink \
        -e "DELETE FROM users WHERE username_normalized = '${TEST_USERNAME}'" \
        >/dev/null 2>&1 || true
    rm -rf "${TMP_DIR}"
}

trap cleanup EXIT

fail()
{
    echo "FAIL: $*" >&2
    docker compose ps >&2 || true
    docker compose logs --tail=100 prometheus grafana shortlink_server nginx >&2 || true
    exit 1
}

expect_contains()
{
    local actual="$1"
    local expected="$2"
    local label="$3"

    if [[ "${actual}" != *"${expected}"* ]]; then
        fail "${label}: expected to contain '${expected}', got '${actual}'"
    fi
}

wait_for_http()
{
    local url="$1"
    local label="$2"

    for _ in {1..60}; do
        if curl -fsS "${url}" >/dev/null 2>&1; then
            echo "PASS: ${label} is ready"
            return 0
        fi
        sleep 1
    done

    fail "${label} did not become ready"
}

prometheus_query()
{
    local query="$1"
    local evaluation_time="${2:-}"

    if [[ -n "${evaluation_time}" ]]; then
        curl -fsS --get \
            --data-urlencode "query=${query}" \
            --data-urlencode "time=${evaluation_time}" \
            "${PROMETHEUS_URL}/api/v1/query"
    else
        curl -fsS --get \
            --data-urlencode "query=${query}" \
            "${PROMETHEUS_URL}/api/v1/query"
    fi
}

wait_for_prometheus_series()
{
    local query="$1"
    local label="$2"

    for _ in {1..30}; do
        local response
        response="$(prometheus_query "${query}")"
        if [[ "${response}" == *'"result":[{'* ]]; then
            echo "PASS: ${label}"
            return 0
        fi
        sleep 1
    done

    fail "${label}: Prometheus query returned no series"
}

command -v docker >/dev/null 2>&1 || fail "docker is required"
command -v curl >/dev/null 2>&1 || fail "curl is required"
docker info >/dev/null 2>&1 || fail "Docker is not available; start OrbStack Docker first"

cd "${ROOT_DIR}"

docker compose up -d --build

wait_for_http "${NGINX_URL}/api/health" "Nginx and shortlink_server"
wait_for_http "${PROMETHEUS_URL}/-/ready" "Prometheus"
wait_for_http "${GRAFANA_URL}/api/health" "Grafana"
wait_for_http "${NGINX_URL}/app/" "management page"
wait_for_http "${NGINX_URL}/openapi.yaml" "OpenAPI document"
management_page="$(curl -fsS "${NGINX_URL}/app/")"
expect_contains "${management_page}" '短链接工作台' "management page content"
expect_contains "${management_page}" 'id="expiry-form"' "management page lifecycle form"
openapi_document="$(curl -fsS "${NGINX_URL}/openapi.yaml")"
expect_contains "${openapi_document}" 'openapi: 3.1.0' "OpenAPI version"
expect_contains "${openapi_document}" '/api/auth/session:' "OpenAPI authentication routes"
echo "PASS: management page and OpenAPI assets are current"

register_status="$(curl -sS -c "${COOKIE_JAR}" -o "${TMP_DIR}/register.body" -w '%{http_code}' \
    -X POST "${NGINX_URL}/api/auth/register" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"${TEST_USERNAME}\",\"password\":\"monitoring-password\"}")"
[[ "${register_status}" == "201" ]] || fail "registration status: expected 201, got ${register_status}"

create_body="$(curl -fsS -X POST "${NGINX_URL}/api/short-links" \
    -b "${COOKIE_JAR}" \
    -H 'Content-Type: application/json' \
    -d "{\"url\":\"${TEST_ORIGINAL_URL}\"}")"
code="$(printf '%s' "${create_body}" | sed -n 's/.*"code":"\([^"]*\)".*/\1/p')"
[[ -n "${code}" ]] || fail "create response did not contain a short code: ${create_body}"

redirect_status="$(curl -sS -o /dev/null -w '%{http_code}' "${NGINX_URL}/s/${code}")"
[[ "${redirect_status}" == "302" ]] || fail "redirect status: expected 302, got ${redirect_status}"
echo "PASS: generated create and redirect traffic"

wait_for_prometheus_series 'up{job="hao-shortlink"} == 1' "Prometheus target is up"
wait_for_prometheus_series 'haohttp_http_requests_total{job="hao-shortlink"}' "Prometheus scraped HTTP metrics"
wait_for_prometheus_series 'haohttp_shortlink_redirect_total{job="hao-shortlink",result="success"}' "Prometheus scraped business metrics"
wait_for_prometheus_series 'sum by (method) (rate(haohttp_http_requests_total[30s]))' "dashboard request-rate query"
wait_for_prometheus_series '100 * sum(rate(haohttp_http_requests_total{status_class="4xx"}[30s])) / clamp_min(sum(rate(haohttp_http_requests_total[30s])), 0.000000001)' "dashboard error-ratio query"
wait_for_prometheus_series 'histogram_quantile(0.95, sum by (le) (rate(haohttp_http_request_duration_seconds_bucket[30s])))' "dashboard latency query"
wait_for_prometheus_series 'sum by (result, source) (rate(haohttp_shortlink_redirect_total[30s]))' "dashboard business query"
wait_for_prometheus_series '100 * sum by (result) (rate(haohttp_shortlink_cache_operations_total{operation="get"}[30s])) / scalar(clamp_min(sum(rate(haohttp_shortlink_cache_operations_total{operation="get"}[30s])), 0.000000001))' "dashboard Redis query"
wait_for_prometheus_series 'sum by (backend, operation) (rate(haohttp_shortlink_backend_errors_total[30s]))' "dashboard backend-error query"

grafana_datasource="$(curl -fsS -u "${GRAFANA_ADMIN_USER}:${GRAFANA_ADMIN_PASSWORD}" \
    "${GRAFANA_URL}/api/datasources/uid/hao-prometheus")"
expect_contains "${grafana_datasource}" '"url":"http://prometheus:9090"' "Grafana Prometheus datasource"
echo "PASS: Grafana datasource was provisioned"

grafana_datasource_health="$(curl -fsS -u "${GRAFANA_ADMIN_USER}:${GRAFANA_ADMIN_PASSWORD}" \
    "${GRAFANA_URL}/api/datasources/uid/hao-prometheus/health")"
expect_contains "${grafana_datasource_health}" '"status":"OK"' "Grafana datasource health"
echo "PASS: Grafana can query Prometheus"

grafana_dashboard="$(curl -fsS -u "${GRAFANA_ADMIN_USER}:${GRAFANA_ADMIN_PASSWORD}" \
    "${GRAFANA_URL}/api/dashboards/uid/hao-shortlink-overview")"
expect_contains "${grafana_dashboard}" '"title":"HaoShortLink Overview"' "Grafana dashboard title"
expect_contains "${grafana_dashboard}" '"title":"HTTP 请求速率"' "Grafana HTTP panel"
expect_contains "${grafana_dashboard}" '"title":"Redis GET 结果占比"' "Grafana Redis panel"
expect_contains "${grafana_dashboard}" '"title":"后端错误速率"' "Grafana backend panel"
echo "PASS: Grafana dashboard was provisioned"

nginx_metrics_status="$(curl -sS -o /dev/null -w '%{http_code}' "${NGINX_URL}/metrics")"
[[ "${nginx_metrics_status}" == "404" ]] || fail "Nginx /metrics: expected 404, got ${nginx_metrics_status}"
echo "PASS: Nginx does not expose /metrics"

sample_time="$(date +%s)"
historical_query='haohttp_shortlink_redirect_total{job="hao-shortlink",result="success"}'
historical_before="$(prometheus_query "${historical_query}" "${sample_time}")"
expect_contains "${historical_before}" '"result":[{' "Prometheus historical sample before recreate"

docker compose up -d --force-recreate --no-deps prometheus
wait_for_http "${PROMETHEUS_URL}/-/ready" "recreated Prometheus"

historical_after="$(prometheus_query "${historical_query}" "${sample_time}")"
expect_contains "${historical_after}" '"result":[{' "Prometheus historical sample after recreate"
echo "PASS: Prometheus history survived container recreation"

docker compose up -d --force-recreate --no-deps grafana
wait_for_http "${GRAFANA_URL}/api/health" "recreated Grafana"

grafana_dashboard="$(curl -fsS -u "${GRAFANA_ADMIN_USER}:${GRAFANA_ADMIN_PASSWORD}" \
    "${GRAFANA_URL}/api/dashboards/uid/hao-shortlink-overview")"
expect_contains "${grafana_dashboard}" '"title":"HaoShortLink Overview"' "Grafana dashboard after recreate"
echo "PASS: Grafana dashboard survived container recreation"

echo "Monitoring smoke test passed"
