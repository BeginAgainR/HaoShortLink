#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
HEY_BIN="${HAOHTTP_HEY_BIN:-hey}"
BENCH_TOOL="${HAOHTTP_BENCH_TOOL:-auto}"
MODE="${HAOHTTP_BENCH_MODE:-memory}"
SCENARIO="${HAOHTTP_BENCH_SCENARIO:-all}"
PORT="${HAOHTTP_BENCH_PORT:-18084}"
BASE_URL="${HAOHTTP_BENCH_BASE_URL:-http://127.0.0.1:${PORT}}"
BASE_URL="${BASE_URL%/}"
USE_EXTERNAL_SERVER=false
if [[ -n "${HAOHTTP_BENCH_BASE_URL:-}" ]]; then
    USE_EXTERNAL_SERVER=true
fi
REQUESTS="${HAOHTTP_BENCH_REQUESTS:-1000}"
CONCURRENCY="${HAOHTTP_BENCH_CONCURRENCY:-16}"
THREAD_NUM="${HAOHTTP_BENCH_THREAD_NUM:-4}"
CURL_MAX_TIME_SECONDS="${HAOHTTP_BENCH_CURL_MAX_TIME_SECONDS:-5}"
MYSQL_POOL_SIZE="${HAOHTTP_BENCH_MYSQL_POOL_SIZE:-4}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
REDIS_HOST="${HAOHTTP_REDIS_HOST:-docker.orb.internal}"
REDIS_PORT="${HAOHTTP_REDIS_PORT:-16379}"
REDIS_KEY_PREFIX="${HAOHTTP_REDIS_KEY_PREFIX:-shortlink:}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-benchmark.XXXXXX")"
CONFIG_FILE="${TMP_DIR}/server.conf"
SERVER_LOG="${TMP_DIR}/shortlink_server.log"
SERVER_PID=""
BENCH_ID="$(date +%s)-${RANDOM}"
BENCH_CREATE_URL="https://example.com/benchmark-create-${BENCH_ID}"
BENCH_REDIRECT_URL="${BASE_URL}/api/health"
REDIRECT_CODE=""
RESOLVED_BENCH_TOOL=""

cleanup()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi

    if [[ "${MODE}" == "mysql" || "${MODE}" == "mysql-redis" ]]; then
        cleanup_mysql_rows || true
        cleanup_redis_key || true
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

usage()
{
    cat <<EOF
Usage: HAOHTTP_BENCH_SCENARIO=health|create|redirect|redirect-cache-hit|redirect-cache-miss|invalid-url|missing-code|all \\
       HAOHTTP_BENCH_MODE=memory|mysql|mysql-redis \\
       bash tests/scripts/benchmark_shortlink.sh

Required:
  Build shortlink_server first. Default binary: ${SERVER_BIN}
  HAOHTTP_BENCH_TOOL=auto uses hey when available, otherwise curl.

Common environment variables:
  HAOHTTP_BENCH_REQUESTS       default: ${REQUESTS}
  HAOHTTP_BENCH_CONCURRENCY   default: ${CONCURRENCY}
  HAOHTTP_BENCH_THREAD_NUM    default: ${THREAD_NUM}
  HAOHTTP_BENCH_PORT          default: ${PORT}
  HAOHTTP_BENCH_BASE_URL      target an already running service or proxy instead of starting shortlink_server
  HAOHTTP_BENCH_CURL_MAX_TIME_SECONDS default: ${CURL_MAX_TIME_SECONDS}
  HAOHTTP_BENCH_MODE          default: ${MODE}
  HAOHTTP_BENCH_SCENARIO      default: ${SCENARIO}
  HAOHTTP_BENCH_TOOL          default: ${BENCH_TOOL}

MySQL / Redis modes use the same HAOHTTP_MYSQL_* and HAOHTTP_REDIS_* variables
as the integration test scripts.
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

cleanup_mysql_rows()
{
    command -v mysql >/dev/null 2>&1 || return 0
    mysql_cmd -e "DELETE FROM short_links WHERE original_url IN ('${BENCH_CREATE_URL}', '${BENCH_REDIRECT_URL}')" \
        >/dev/null 2>&1 || true
}

cleanup_redis_key()
{
    command -v redis-cli >/dev/null 2>&1 || return 0
    [[ -n "${REDIRECT_CODE}" ]] || return 0
    redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY_PREFIX}${REDIRECT_CODE}" \
        >/dev/null 2>&1 || true
}

delete_redis_key()
{
    local code="$1"
    command -v redis-cli >/dev/null 2>&1 || fail "redis-cli is required for Redis cache benchmark scenarios"
    redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY_PREFIX}${code}" >/dev/null
}

write_config()
{
    cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkBenchmark
server.port=${PORT}
server.thread_num=${THREAD_NUM}
storage.type=memory
redis.enabled=false
EOF

    case "${MODE}" in
        memory)
            ;;
        mysql|mysql-redis)
            cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkBenchmark
server.port=${PORT}
server.thread_num=${THREAD_NUM}
storage.type=mysql
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=${MYSQL_POOL_SIZE}
redis.enabled=false
EOF
            if [[ "${MODE}" == "mysql-redis" ]]; then
                cat >> "${CONFIG_FILE}" <<EOF
redis.enabled=true
redis.host=${REDIS_HOST}
redis.port=${REDIS_PORT}
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=${REDIS_KEY_PREFIX}
EOF
            fi
            ;;
        *)
            fail "unsupported HAOHTTP_BENCH_MODE=${MODE}; use memory, mysql, or mysql-redis"
            ;;
    esac
}

wait_for_ready()
{
    for _ in {1..80}; do
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

wait_for_external_ready()
{
    for _ in {1..80}; do
        if curl -fsS "${BASE_URL}/api/health" >/dev/null 2>&1; then
            return 0
        fi

        sleep 0.1
    done

    fail "external benchmark target did not become ready at ${BASE_URL}"
}

ensure_benchmark_port_unused()
{
    if curl -fsS --max-time 1 "${BASE_URL}/api/health" >/dev/null 2>&1; then
        fail "benchmark port ${PORT} already has a shortlink_server health endpoint; stop the existing process or set HAOHTTP_BENCH_PORT"
    fi
}

create_short_link()
{
    local original_url="$1"
    local body_file="${TMP_DIR}/create-body.txt"
    local status

    status="$(curl -sS -o "${body_file}" -w "%{http_code}" \
        -X POST "${BASE_URL}/api/short-links" \
        -H 'Content-Type: application/json' \
        -d "{\"url\":\"${original_url}\"}")"

    if [[ "${status}" != "201" ]]; then
        fail "failed to create benchmark short link for ${original_url}: HTTP ${status}, body $(cat "${body_file}")"
    fi

    sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}"
}

warm_redirect_cache()
{
    local code="$1"
    local status

    status="$(curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -o /dev/null -w "%{http_code}" "${BASE_URL}/s/${code}")"
    if [[ "${status}" != "302" ]]; then
        fail "failed to warm redirect cache for ${code}: HTTP ${status}"
    fi
}

hey_supports_disable_redirects()
{
    { "${HEY_BIN}" 2>&1 || true; } | grep -q -- "-disable-redirects"
}

safe_file_name()
{
    printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_'
}

resolve_bench_tool()
{
    case "${BENCH_TOOL}" in
        auto)
            if command -v "${HEY_BIN}" >/dev/null 2>&1; then
                RESOLVED_BENCH_TOOL="hey"
            else
                RESOLVED_BENCH_TOOL="curl"
            fi
            ;;
        hey)
            command -v "${HEY_BIN}" >/dev/null 2>&1 || fail "hey is required; install hey or set HAOHTTP_HEY_BIN"
            RESOLVED_BENCH_TOOL="hey"
            ;;
        curl)
            RESOLVED_BENCH_TOOL="curl"
            ;;
        *)
            fail "unsupported HAOHTTP_BENCH_TOOL=${BENCH_TOOL}; use auto, hey, or curl"
            ;;
    esac
}

run_benchmark()
{
    local label="$1"
    local expected_status="$2"
    local method="$3"
    local path="$4"
    local body="${5:-}"

    case "${RESOLVED_BENCH_TOOL}" in
        hey)
            run_hey "${label}" "${expected_status}" "${method}" "${path}" "${body}"
            ;;
        curl)
            run_curl_benchmark "${label}" "${expected_status}" "${method}" "${path}" "${body}"
            ;;
        *)
            fail "benchmark tool was not resolved"
            ;;
    esac
}

run_hey()
{
    local label="$1"
    local expected_status="$2"
    local method="$3"
    local path="$4"
    local body="${5:-}"
    local safe_label
    safe_label="$(safe_file_name "${label}")"
    local output_file="${TMP_DIR}/${safe_label}.hey.csv"
    local url="${BASE_URL}${path}"
    local -a cmd=("${HEY_BIN}" -n "${REQUESTS}" -c "${CONCURRENCY}" -o csv -m "${method}")

    if [[ "${method}" == "POST" ]]; then
        cmd+=(-H "Content-Type: application/json" -d "${body}")
    fi

    if [[ "${expected_status}" == "302" ]] && hey_supports_disable_redirects; then
        cmd+=(-disable-redirects)
    fi

    cmd+=("${url}")

    if ! "${cmd[@]}" > "${output_file}" 2>&1; then
        cat "${output_file}" >&2 || true
        fail "hey failed for ${label}"
    fi

    print_hey_result_row "${label}" "${expected_status}" "${output_file}"
}

print_hey_result_row()
{
    local label="$1"
    local expected_status="$2"
    local output_file="$3"
    local latency_file="${output_file}.latency"
    local sorted_latency_file="${output_file}.latency.sorted"
    local qps average p95 p99 total expected_count total_duration error_rate note target_context

    awk -F, 'NR > 1 { print $1 }' "${output_file}" > "${latency_file}"
    sort -n "${latency_file}" > "${sorted_latency_file}"

    total="$(wc -l < "${latency_file}" | tr -d ' ')"
    expected_count="$(awk -F, -v code="${expected_status}" 'NR > 1 && $7 == code { count++ } END { print count + 0 }' "${output_file}")"
    total_duration="$(awk -F, 'NR > 1 { end = $8 + $1; if (end > max) max = end } END { printf "%.6f", max + 0 }' "${output_file}")"

    if awk -v duration="${total_duration}" 'BEGIN { exit !(duration > 0) }'; then
        qps="$(awk -v total="${total}" -v duration="${total_duration}" 'BEGIN { printf "%.2f", total / duration }')"
    else
        qps="unknown"
    fi

    average="$(awk '{ sum += $1 } END { if (NR > 0) printf "%.6fs", sum / NR; else print "unknown" }' "${latency_file}")"
    p95="$(percentile_from_sorted_file "${sorted_latency_file}" "${total}" 95)"
    p99="$(percentile_from_sorted_file "${sorted_latency_file}" "${total}" 99)"
    if [[ "${p95}" != "unknown" ]]; then
        p95="${p95}s"
    fi
    if [[ "${p99}" != "unknown" ]]; then
        p99="${p99}s"
    fi

    if [[ "${total}" -gt 0 ]]; then
        error_rate="$(awk -v total="${total}" -v ok="${expected_count}" 'BEGIN { printf "%.2f%%", ((total - ok) * 100.0 / total) }')"
    else
        error_rate="unknown"
    fi

    target_context="commit $(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    if [[ "${USE_EXTERNAL_SERVER}" == true ]]; then
        target_context="external target ${BASE_URL}; repo ${target_context}"
    fi
    note="tool hey; expected ${expected_status}; ${target_context}"
    if [[ "${expected_status}" == "302" ]] && ! hey_supports_disable_redirects; then
        note="${note}; hey followed redirects"
    fi

    echo "| ${label} | ${MODE} | ${CONCURRENCY} | ${REQUESTS} requests | ${qps:-unknown} | ${average:-unknown} | ${p95:-unknown} | ${p99:-unknown} | ${error_rate} | ${note} |"
}

now_ms()
{
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import time; print(int(time.time() * 1000))'
    else
        printf '%s000\n' "$(date +%s)"
    fi
}

run_curl_benchmark()
{
    local label="$1"
    local expected_status="$2"
    local method="$3"
    local path="$4"
    local body="${5:-}"
    local safe_label
    safe_label="$(safe_file_name "${label}")"
    local output_dir="${TMP_DIR}/${safe_label}.curl"
    local output_file="${TMP_DIR}/${safe_label}.curl.txt"
    local url="${BASE_URL}${path}"
    local start_ms end_ms elapsed_ms
    local curl_worker

    mkdir -p "${output_dir}"

    curl_worker='
method="$1"
url="$2"
body="$3"
output_dir="$4"
max_time="$5"
idx="$6"
output_file="${output_dir}/${idx}.txt"

if [ "${method}" = "POST" ]; then
    curl -sS --max-time "${max_time}" -o /dev/null -w "%{http_code} %{time_total}\n" \
        -X "${method}" -H "Content-Type: application/json" -d "${body}" "${url}" \
        > "${output_file}" 2>/dev/null || echo "000 0" > "${output_file}"
else
    curl -sS --max-time "${max_time}" -o /dev/null -w "%{http_code} %{time_total}\n" \
        -X "${method}" "${url}" \
        > "${output_file}" 2>/dev/null || echo "000 0" > "${output_file}"
fi
'

    start_ms="$(now_ms)"
    seq 1 "${REQUESTS}" |
        xargs -n 1 -P "${CONCURRENCY}" sh -c "${curl_worker}" sh \
            "${method}" "${url}" "${body}" "${output_dir}" "${CURL_MAX_TIME_SECONDS}"
    end_ms="$(now_ms)"
    elapsed_ms=$((end_ms - start_ms))

    cat "${output_dir}"/*.txt > "${output_file}"
    print_curl_result_row "${label}" "${expected_status}" "${output_file}" "${elapsed_ms}"
}

percentile_from_sorted_file()
{
    local sorted_file="$1"
    local count="$2"
    local percentile="$3"
    local index

    if [[ "${count}" -le 0 ]]; then
        echo "unknown"
        return
    fi

    index="$(awk -v n="${count}" -v p="${percentile}" 'BEGIN {
        idx = int((n * p + 99) / 100);
        if (idx < 1) idx = 1;
        if (idx > n) idx = n;
        print idx;
    }')"

    sed -n "${index}p" "${sorted_file}"
}

print_curl_result_row()
{
    local label="$1"
    local expected_status="$2"
    local output_file="$3"
    local elapsed_ms="$4"
    local latency_file="${output_file}.latency"
    local sorted_latency_file="${output_file}.latency.sorted"
    local total expected_count qps average p95 p99 error_rate note target_context

    total="$(wc -l < "${output_file}" | tr -d ' ')"
    expected_count="$(awk -v code="${expected_status}" '$1 == code { count++ } END { print count + 0 }' "${output_file}")"
    awk '{ print $2 }' "${output_file}" > "${latency_file}"
    sort -n "${latency_file}" > "${sorted_latency_file}"

    if [[ "${elapsed_ms}" -gt 0 ]]; then
        qps="$(awk -v total="${total}" -v elapsed_ms="${elapsed_ms}" 'BEGIN { printf "%.2f", total * 1000.0 / elapsed_ms }')"
    else
        qps="unknown"
    fi

    average="$(awk '{ sum += $1 } END { if (NR > 0) printf "%.6fs", sum / NR; else print "unknown" }' "${latency_file}")"
    p95="$(percentile_from_sorted_file "${sorted_latency_file}" "${total}" 95)"
    p99="$(percentile_from_sorted_file "${sorted_latency_file}" "${total}" 99)"
    if [[ "${p95}" != "unknown" ]]; then
        p95="${p95}s"
    fi
    if [[ "${p99}" != "unknown" ]]; then
        p99="${p99}s"
    fi

    if [[ "${total}" -gt 0 ]]; then
        error_rate="$(awk -v total="${total}" -v ok="${expected_count}" 'BEGIN { printf "%.2f%%", ((total - ok) * 100.0 / total) }')"
    else
        error_rate="unknown"
    fi

    target_context="commit $(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    if [[ "${USE_EXTERNAL_SERVER}" == true ]]; then
        target_context="external target ${BASE_URL}; repo ${target_context}"
    fi
    note="tool curl; expected ${expected_status}; ${target_context}"
    echo "| ${label} | ${MODE} | ${CONCURRENCY} | ${REQUESTS} requests | ${qps:-unknown} | ${average:-unknown} | ${p95:-unknown} | ${p99:-unknown} | ${error_rate} | ${note} |"
}

run_scenario()
{
    local scenario="$1"

    case "${scenario}" in
        health)
            run_benchmark "GET /api/health" "200" "GET" "/api/health"
            ;;
        create)
            run_benchmark "POST /api/short-links" "201" "POST" "/api/short-links" "{\"url\":\"${BENCH_CREATE_URL}\"}"
            ;;
        redirect)
            REDIRECT_CODE="$(create_short_link "${BENCH_REDIRECT_URL}")"
            [[ -n "${REDIRECT_CODE}" ]] || fail "created redirect benchmark link did not return a code"
            run_benchmark "GET /s/{code}" "302" "GET" "/s/${REDIRECT_CODE}"
            ;;
        redirect-cache-hit)
            [[ "${MODE}" == "mysql-redis" ]] || fail "redirect-cache-hit requires HAOHTTP_BENCH_MODE=mysql-redis"
            REDIRECT_CODE="$(create_short_link "${BENCH_REDIRECT_URL}")"
            [[ -n "${REDIRECT_CODE}" ]] || fail "created redirect benchmark link did not return a code"
            warm_redirect_cache "${REDIRECT_CODE}"
            run_benchmark "GET /s/{code} Redis hit" "302" "GET" "/s/${REDIRECT_CODE}"
            ;;
        redirect-cache-miss)
            [[ "${MODE}" == "mysql-redis" ]] || fail "redirect-cache-miss requires HAOHTTP_BENCH_MODE=mysql-redis"
            REDIRECT_CODE="$(create_short_link "${BENCH_REDIRECT_URL}")"
            [[ -n "${REDIRECT_CODE}" ]] || fail "created redirect benchmark link did not return a code"
            delete_redis_key "${REDIRECT_CODE}"
            local original_requests="${REQUESTS}"
            local original_concurrency="${CONCURRENCY}"
            REQUESTS=1
            CONCURRENCY=1
            run_benchmark "GET /s/{code} Redis miss" "302" "GET" "/s/${REDIRECT_CODE}"
            REQUESTS="${original_requests}"
            CONCURRENCY="${original_concurrency}"
            ;;
        invalid-url)
            run_benchmark "POST invalid URL" "400" "POST" "/api/short-links" '{"url":"ftp://example.com/not-supported"}'
            ;;
        missing-code)
            run_benchmark "GET missing short code" "404" "GET" "/s/notfound-benchmark"
            ;;
        all)
            run_scenario health
            run_scenario create
            run_scenario redirect
            run_scenario invalid-url
            run_scenario missing-code
            ;;
        *)
            fail "unsupported HAOHTTP_BENCH_SCENARIO=${scenario}"
            ;;
    esac
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

trap cleanup EXIT

if [[ "${USE_EXTERNAL_SERVER}" == false && ! -x "${SERVER_BIN}" ]]; then
    fail "shortlink_server binary not found or not executable at ${SERVER_BIN}; build the project first"
fi

command -v curl >/dev/null 2>&1 || fail "curl is required"

resolve_bench_tool
if [[ "${USE_EXTERNAL_SERVER}" == true ]]; then
    wait_for_external_ready
    CONFIG_DESCRIPTION="external target ${BASE_URL}"
else
    write_config
    ensure_benchmark_port_unused
    "${SERVER_BIN}" "${CONFIG_FILE}" > "${SERVER_LOG}" 2>&1 < /dev/null &
    SERVER_PID="$!"
    wait_for_ready
    CONFIG_DESCRIPTION="${CONFIG_FILE}"
fi

cat <<EOF
Benchmark configuration:
- mode: ${MODE}
- scenario: ${SCENARIO}
- requests: ${REQUESTS}
- concurrency: ${CONCURRENCY}
- tool: ${RESOLVED_BENCH_TOOL}
- server.thread_num: ${THREAD_NUM}
- mysql.pool_size: ${MYSQL_POOL_SIZE}
- config: ${CONFIG_DESCRIPTION}

| 场景 | 模式 | 并发 | 总请求/时长 | QPS | 平均延迟 | P95 | P99 | 错误率 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
EOF

run_scenario "${SCENARIO}"
