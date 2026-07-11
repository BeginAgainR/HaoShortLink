#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_CREATE_DIAG_PORT:-18086}"
BASE_URL="http://127.0.0.1:${PORT}"
REQUESTS="${HAOHTTP_CREATE_DIAG_REQUESTS:-100}"
CONCURRENCY_LEVELS="${HAOHTTP_CREATE_DIAG_CONCURRENCY_LEVELS:-1 2 4 8 16}"
THREAD_NUM="${HAOHTTP_CREATE_DIAG_THREAD_NUM:-4}"
MYSQL_POOL_SIZE="${HAOHTTP_CREATE_DIAG_MYSQL_POOL_SIZE:-4}"
CURL_MAX_TIME_SECONDS="${HAOHTTP_CREATE_DIAG_CURL_MAX_TIME_SECONDS:-10}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
ARTIFACT_DIR="${HAOHTTP_CREATE_DIAG_ARTIFACT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/haohttp-create-diagnostic.XXXXXX")}"
CONFIG_FILE="${ARTIFACT_DIR}/server.conf"
SERVER_LOG="${ARTIFACT_DIR}/shortlink_server.log"
SUMMARY_FILE="${ARTIFACT_DIR}/summary.md"
SERVER_PID=""
RUN_ID="$(date +%s)-${RANDOM}"
URL_PREFIX="https://example.com/mysql-create-diagnostic-${RUN_ID}"

cleanup()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi

    cleanup_mysql_rows || true
}

fail()
{
    echo "FAIL: $*" >&2
    if [[ -f "${SERVER_LOG}" ]]; then
        echo "---- shortlink_server log ----" >&2
        tail -n 120 "${SERVER_LOG}" >&2 || true
        echo "------------------------------" >&2
    fi
    echo "Artifacts preserved at: ${ARTIFACT_DIR}" >&2
    exit 1
}

usage()
{
    cat <<EOF
Usage: bash tests/scripts/mysql_create_concurrency_diagnostic.sh

Diagnoses BUG-004 by running POST /api/short-links against MySQL storage across
concurrency levels and preserving status distributions, response bodies, config,
and shortlink_server logs.

Common environment variables:
  HAOHTTP_CREATE_DIAG_REQUESTS             default: ${REQUESTS}
  HAOHTTP_CREATE_DIAG_CONCURRENCY_LEVELS   default: ${CONCURRENCY_LEVELS}
  HAOHTTP_CREATE_DIAG_THREAD_NUM           default: ${THREAD_NUM}
  HAOHTTP_CREATE_DIAG_MYSQL_POOL_SIZE      default: ${MYSQL_POOL_SIZE}
  HAOHTTP_CREATE_DIAG_PORT                 default: ${PORT}
  HAOHTTP_CREATE_DIAG_ARTIFACT_DIR         default: auto-created under /tmp
  HAOHTTP_CREATE_DIAG_CURL_MAX_TIME_SECONDS default: ${CURL_MAX_TIME_SECONDS}

MySQL connection variables follow the integration scripts:
  HAOHTTP_MYSQL_HOST, HAOHTTP_MYSQL_PORT, HAOHTTP_MYSQL_USER,
  HAOHTTP_MYSQL_PASSWORD, HAOHTTP_MYSQL_DATABASE.
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
    mysql_cmd -e "DELETE FROM short_links WHERE original_url LIKE '${URL_PREFIX}%'" >/dev/null 2>&1 || true
}

write_config()
{
    mkdir -p "${ARTIFACT_DIR}"
    cat > "${CONFIG_FILE}" <<EOF
server.name=HaoShortLinkCreateDiagnostic
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
}

ensure_port_unused()
{
    if curl -fsS --max-time 1 "${BASE_URL}/api/health" >/dev/null 2>&1; then
        fail "diagnostic port ${PORT} already has a shortlink_server health endpoint; stop it or set HAOHTTP_CREATE_DIAG_PORT"
    fi
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

run_level()
{
    local concurrency="$1"
    local level_dir="${ARTIFACT_DIR}/concurrency-${concurrency}"
    local bodies_dir="${level_dir}/bodies"
    local status_dir="${level_dir}/statuses"
    local status_file="${level_dir}/statuses.txt"
    local failures_file="${level_dir}/failure_samples.txt"
    local worker

    mkdir -p "${bodies_dir}" "${status_dir}"

    worker='
base_url="$1"
bodies_dir="$2"
status_dir="$3"
url_prefix="$4"
max_time="$5"
concurrency="$6"
idx="$7"
body_file="${bodies_dir}/${idx}.body"
status_file="${status_dir}/${idx}.status"
original_url="${url_prefix}-c${concurrency}-r${idx}"
payload="{\"url\":\"${original_url}\"}"

line="$(curl -sS --max-time "${max_time}" -o "${body_file}" -w "%{http_code} %{time_total}" \
    -X POST "${base_url}/api/short-links" \
    -H "Content-Type: application/json" \
    -d "${payload}" 2>/dev/null || printf "000 0")"
printf "%s %s %s\n" "${line}" "${idx}" "${body_file}" > "${status_file}"
'

    seq 1 "${REQUESTS}" |
        xargs -n 1 -P "${concurrency}" sh -c "${worker}" sh \
            "${BASE_URL}" "${bodies_dir}" "${status_dir}" "${URL_PREFIX}" \
            "${CURL_MAX_TIME_SECONDS}" "${concurrency}"

    cat "${status_dir}"/*.status | sort -n -k3 > "${status_file}"

    {
        echo "## concurrency=${concurrency}"
        echo
        echo "Status distribution:"
        awk '{ count[$1]++ } END { for (code in count) print code, count[code] }' "${status_file}" | sort
        echo
        echo "Non-201 samples:"
    } | tee -a "${SUMMARY_FILE}"

    awk '$1 != "201" { print $0 }' "${status_file}" | head -n 10 > "${failures_file}"
    if [[ -s "${failures_file}" ]]; then
        while read -r status time idx body_file; do
            {
                echo "- status=${status} time=${time} request=${idx} body_file=${body_file}"
                sed 's/^/  body: /' "${body_file}" | head -n 5
            } | tee -a "${SUMMARY_FILE}"
        done < "${failures_file}"
    else
        echo "- none" | tee -a "${SUMMARY_FILE}"
    fi

    echo | tee -a "${SUMMARY_FILE}"
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

mysql_cmd -e "SELECT 1" >/dev/null 2>&1 ||
    fail "MySQL is not reachable at ${MYSQL_HOST}:${MYSQL_PORT}; start Compose mysql dependency first"

trap cleanup EXIT

write_config
ensure_port_unused

cat > "${SUMMARY_FILE}" <<EOF
# MySQL create concurrency diagnostic

- commit: $(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)
- requests per level: ${REQUESTS}
- concurrency levels: ${CONCURRENCY_LEVELS}
- server.thread_num: ${THREAD_NUM}
- mysql.pool_size: ${MYSQL_POOL_SIZE}
- port: ${PORT}
- config: ${CONFIG_FILE}
- server log: ${SERVER_LOG}
- url prefix: ${URL_PREFIX}

EOF

"${SERVER_BIN}" "${CONFIG_FILE}" > "${SERVER_LOG}" 2>&1 < /dev/null &
SERVER_PID="$!"
wait_for_ready

echo "Artifacts: ${ARTIFACT_DIR}"
echo "Summary: ${SUMMARY_FILE}"
echo

for concurrency in ${CONCURRENCY_LEVELS}; do
    echo "Running concurrency=${concurrency}, requests=${REQUESTS}"
    run_level "${concurrency}"
done

echo "Diagnostic complete."
echo "Artifacts preserved at: ${ARTIFACT_DIR}"
