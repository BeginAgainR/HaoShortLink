#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MYSQL_IMAGE="${SHORTLINK_MYSQL_IMAGE:-docker.m.daocloud.io/library/mysql:8.0}"
REDIS_IMAGE="${SHORTLINK_REDIS_IMAGE:-docker.m.daocloud.io/library/redis:7-alpine}"
TEST_HOST="${HAOHTTP_TEST_HOST:-}"
TEST_WORKDIR="${HAOHTTP_TEST_WORKDIR:-${ROOT_DIR}}"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

wait_for_healthy()
{
    local container_name="$1"
    local label="$2"

    for _ in {1..60}; do
        local status
        status="$(docker inspect -f '{{.State.Health.Status}}' "${container_name}" 2>/dev/null || true)"
        if [[ "${status}" == "healthy" ]]; then
            echo "PASS: ${label} is healthy"
            return 0
        fi

        sleep 1
    done

    docker compose ps >&2 || true
    fail "${label} did not become healthy"
}

run_test_script()
{
    local script_path="$1"

    if [[ -n "${TEST_HOST}" ]]; then
        ssh "${TEST_HOST}" "cd '${TEST_WORKDIR}' && bash '${script_path}'"
    else
        bash "${script_path}"
    fi
}

command -v docker >/dev/null 2>&1 || fail "docker is required"
docker info >/dev/null 2>&1 || fail "Docker is not available; start OrbStack Docker first"

cd "${ROOT_DIR}"

SHORTLINK_MYSQL_IMAGE="${MYSQL_IMAGE}" \
SHORTLINK_REDIS_IMAGE="${REDIS_IMAGE}" \
docker compose up -d mysql redis

wait_for_healthy "hao-shortlink-mysql" "MySQL"
wait_for_healthy "hao-shortlink-redis" "Redis"

run_test_script "tests/scripts/mysql_redis_integration_test.sh"
run_test_script "tests/scripts/redis_unavailable_fallback_test.sh"
run_test_script "tests/scripts/rate_limit_test.sh"

HAOHTTP_TEST_HOST="${TEST_HOST}" \
HAOHTTP_TEST_WORKDIR="${TEST_WORKDIR}" \
bash tests/scripts/mysql_readiness_test.sh

echo "Compose-backed integration tests passed"
