#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_READINESS_PORT:-18088}"
BASE_URL="http://127.0.0.1:${PORT}"
TEST_HOST="${HAOHTTP_TEST_HOST:-}"
TEST_WORKDIR="${HAOHTTP_TEST_WORKDIR:-${ROOT_DIR}}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/haohttp-readiness-orchestrator.XXXXXX")"
LOCAL_CONFIG="${TMP_DIR}/server.conf"
REMOTE_TMP_DIR=""
REMOTE_CONFIG=""
REMOTE_LOG=""
SERVER_PID=""

fail()
{
    echo "FAIL: $*" >&2
    if [[ -n "${REMOTE_LOG}" ]]; then
        run_on_test_host "tail -n 100 '${REMOTE_LOG}'" >&2 || true
    fi
    exit 1
}

run_on_test_host()
{
    local command="$1"
    if [[ -n "${TEST_HOST}" ]]; then
        ssh "${TEST_HOST}" "${command}"
    else
        bash -lc "${command}"
    fi
}

copy_to_test_host()
{
    local source="$1"
    local destination="$2"
    if [[ -n "${TEST_HOST}" ]]; then
        scp -q "${source}" "${TEST_HOST}:${destination}"
    else
        cp "${source}" "${destination}"
    fi
}

wait_for_mysql_container()
{
    for _ in {1..60}; do
        status="$(docker inspect -f '{{.State.Health.Status}}' hao-shortlink-mysql 2>/dev/null || true)"
        if [[ "${status}" == "healthy" ]]; then
            return 0
        fi
        sleep 1
    done
    fail "MySQL container did not become healthy"
}

remote_status()
{
    local path="$1"
    run_on_test_host "curl -sS --max-time 5 -o /dev/null -w '%{http_code}' '${BASE_URL}${path}'"
}

wait_for_remote_status()
{
    local path="$1"
    local expected="$2"
    for _ in {1..50}; do
        if [[ "$(remote_status "${path}" 2>/dev/null || true)" == "${expected}" ]]; then
            return 0
        fi
        sleep 0.2
    done
    fail "${path} did not return ${expected}"
}

cleanup()
{
    if [[ -n "${SERVER_PID}" ]]; then
        run_on_test_host "kill '${SERVER_PID}' 2>/dev/null || true" || true
    fi
    if [[ -n "${REMOTE_TMP_DIR}" ]]; then
        run_on_test_host "rm -rf '${REMOTE_TMP_DIR}'" || true
    fi
    docker compose up -d mysql >/dev/null 2>&1 || true
    rm -rf "${TMP_DIR}"
}

trap cleanup EXIT

command -v docker >/dev/null 2>&1 || fail "docker is required on the orchestration host"
docker info >/dev/null 2>&1 || fail "Docker is not available"
if [[ -n "${TEST_HOST}" ]]; then
    command -v ssh >/dev/null 2>&1 || fail "ssh is required"
    command -v scp >/dev/null 2>&1 || fail "scp is required"
fi

cd "${ROOT_DIR}"
docker compose up -d mysql
wait_for_mysql_container

cat > "${LOCAL_CONFIG}" <<EOF
server.name=HaoShortLinkReadinessTest
server.port=${PORT}
server.thread_num=2
metrics.enabled=true
storage.type=mysql
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=2
redis.enabled=false
rate_limit.enabled=false
EOF

REMOTE_TMP_DIR="$(run_on_test_host "mktemp -d /tmp/haohttp-readiness.XXXXXX")"
REMOTE_CONFIG="${REMOTE_TMP_DIR}/server.conf"
REMOTE_LOG="${REMOTE_TMP_DIR}/shortlink_server.log"
copy_to_test_host "${LOCAL_CONFIG}" "${REMOTE_CONFIG}"

SERVER_PID="$(run_on_test_host "cd '${TEST_WORKDIR}' && nohup '${SERVER_BIN}' '${REMOTE_CONFIG}' > '${REMOTE_LOG}' 2>&1 & echo \$!")"
[[ "${SERVER_PID}" =~ ^[0-9]+$ ]] || fail "failed to start shortlink_server on test host"

wait_for_remote_status "/api/health/ready" "200"
echo "PASS: MySQL-backed readiness starts at 200"

docker compose stop mysql
wait_for_remote_status "/api/health/ready" "503"
if [[ "$(remote_status "/api/health/live")" != "200" ]]; then
    fail "liveness should remain 200 while MySQL is unavailable"
fi
echo "PASS: MySQL outage changes readiness to 503 while liveness remains 200"

docker compose up -d mysql
wait_for_mysql_container
wait_for_remote_status "/api/health/ready" "200"
echo "PASS: readiness recovers after MySQL returns"

echo "MySQL readiness dependency test passed"
