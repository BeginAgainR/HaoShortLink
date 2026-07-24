#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${HAOHTTP_BUILD_DIR:-/tmp/haoHTTP-build}"
SERVER_BIN="${HAOHTTP_SERVER_BIN:-${BUILD_DIR}/shortlink_server}"
PORT="${HAOHTTP_REDIS_DIAG_PORT:-18089}"
BASE_URL="http://127.0.0.1:${PORT}"
REQUESTS="${HAOHTTP_REDIS_DIAG_REQUESTS:-100}"
THREAD_NUM="${HAOHTTP_REDIS_DIAG_THREAD_NUM:-4}"
MYSQL_POOL_SIZE="${HAOHTTP_REDIS_DIAG_MYSQL_POOL_SIZE:-4}"
CURL_MAX_TIME_SECONDS="${HAOHTTP_REDIS_DIAG_CURL_MAX_TIME_SECONDS:-5}"
MYSQL_HOST="${HAOHTTP_MYSQL_HOST:-docker.orb.internal}"
MYSQL_PORT="${HAOHTTP_MYSQL_PORT:-13306}"
MYSQL_USER="${HAOHTTP_MYSQL_USER:-hao_shortlink}"
MYSQL_PASSWORD="${HAOHTTP_MYSQL_PASSWORD:-hao_shortlink}"
MYSQL_DATABASE="${HAOHTTP_MYSQL_DATABASE:-hao_shortlink}"
REDIS_HOST="${HAOHTTP_REDIS_HOST:-docker.orb.internal}"
REDIS_PORT="${HAOHTTP_REDIS_PORT:-16379}"
REDIS_KEY_PREFIX="${HAOHTTP_REDIS_KEY_PREFIX:-shortlink:}"
ARTIFACT_DIR="${HAOHTTP_REDIS_DIAG_ARTIFACT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/haohttp-redis-diagnostic.XXXXXX")}"
SUMMARY_FILE="${ARTIFACT_DIR}/summary.md"
REDIS_PROBE="${ARTIFACT_DIR}/redis_probe.py"
SERVER_PID=""
CURRENT_SERVER_LOG=""
RUN_ID="$(date +%s)-${RANDOM}"
ORIGINAL_URL="https://example.com/redis-diagnostic-${RUN_ID}"
REDIRECT_CODE=""
AUTH_SEQUENCE=0
COOKIE_JAR="${ARTIFACT_DIR}/session.cookies"

cleanup()
{
    stop_server

    if command -v redis-cli >/dev/null 2>&1 && [[ -n "${REDIRECT_CODE}" ]]; then
        redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY_PREFIX}${REDIRECT_CODE}" >/dev/null 2>&1 || true
    fi

    if command -v mysql >/dev/null 2>&1; then
        mysql_cmd -e "DELETE FROM short_links WHERE original_url = '${ORIGINAL_URL}'" >/dev/null 2>&1 || true
    fi
}

fail()
{
    echo "FAIL: $*" >&2
    if [[ -n "${CURRENT_SERVER_LOG}" && -f "${CURRENT_SERVER_LOG}" ]]; then
        echo "---- shortlink_server log ----" >&2
        tail -n 120 "${CURRENT_SERVER_LOG}" >&2 || true
        echo "------------------------------" >&2
    fi
    echo "Artifacts preserved at: ${ARTIFACT_DIR}" >&2
    exit 1
}

usage()
{
    cat <<EOF
Usage: bash tests/scripts/redis_cache_diagnostic.sh

Runs v1.4.5 Redis cache diagnostics. It compares direct Redis commands,
HTTP Redis hit/miss paths, missing-code behavior, and pure MySQL redirect
baselines.

Common environment variables:
  HAOHTTP_REDIS_DIAG_REQUESTS              default: ${REQUESTS}
  HAOHTTP_REDIS_DIAG_PORT                  default: ${PORT}
  HAOHTTP_REDIS_DIAG_THREAD_NUM            default: ${THREAD_NUM}
  HAOHTTP_REDIS_DIAG_MYSQL_POOL_SIZE       default: ${MYSQL_POOL_SIZE}
  HAOHTTP_REDIS_DIAG_ARTIFACT_DIR          default: auto-created under /tmp
  HAOHTTP_REDIS_DIAG_CURL_MAX_TIME_SECONDS default: ${CURL_MAX_TIME_SECONDS}

MySQL / Redis variables follow the other integration scripts:
  HAOHTTP_MYSQL_HOST, HAOHTTP_MYSQL_PORT, HAOHTTP_MYSQL_USER,
  HAOHTTP_MYSQL_PASSWORD, HAOHTTP_MYSQL_DATABASE,
  HAOHTTP_REDIS_HOST, HAOHTTP_REDIS_PORT, HAOHTTP_REDIS_KEY_PREFIX.
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

stop_server()
{
    if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
    SERVER_PID=""
}

write_config()
{
    local config_file="$1"
    local redis_enabled="$2"

    cat > "${config_file}" <<EOF
server.name=HaoShortLinkRedisDiagnostic
server.port=${PORT}
server.thread_num=${THREAD_NUM}
storage.type=mysql
mysql.host=tcp://${MYSQL_HOST}:${MYSQL_PORT}
mysql.user=${MYSQL_USER}
mysql.password=${MYSQL_PASSWORD}
mysql.database=${MYSQL_DATABASE}
mysql.pool_size=${MYSQL_POOL_SIZE}
redis.enabled=${redis_enabled}
redis.host=${REDIS_HOST}
redis.port=${REDIS_PORT}
redis.database=0
redis.ttl_seconds=3600
redis.key_prefix=${REDIS_KEY_PREFIX}
auth.registration_enabled=true
auth.session_ttl_seconds=3600
auth.cookie_secure=false
EOF
}

establish_session()
{
    AUTH_SEQUENCE=$((AUTH_SEQUENCE + 1))
    local username="redisdiag${RUN_ID//-/}${AUTH_SEQUENCE}"
    local status
    status="$(curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" \
        -o "${ARTIFACT_DIR}/auth-${AUTH_SEQUENCE}.body" \
        -w "%{http_code}" \
        -c "${COOKIE_JAR}" \
        -H "Content-Type: application/json" \
        -d "{\"username\":\"${username}\",\"password\":\"redis-diagnostic-password\"}" \
        "${BASE_URL}/api/auth/register")"
    expect_eq "${status}" "201" "register Redis diagnostic user"
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

start_server()
{
    local redis_enabled="$1"
    local name="$2"
    local server_dir="${ARTIFACT_DIR}/${name}"
    local config_file="${server_dir}/server.conf"

    stop_server
    mkdir -p "${server_dir}"
    CURRENT_SERVER_LOG="${server_dir}/shortlink_server.log"
    write_config "${config_file}" "${redis_enabled}"

    "${SERVER_BIN}" "${config_file}" > "${CURRENT_SERVER_LOG}" 2>&1 < /dev/null &
    SERVER_PID="$!"
    wait_for_ready
    establish_session
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

append_result_row()
{
    local label="$1"
    local expected_status="$2"
    local output_file="$3"
    local note="$4"
    local latency_file="${output_file}.latency"
    local sorted_latency_file="${output_file}.latency.sorted"
    local total expected_count average p95 p99 error_rate

    total="$(wc -l < "${output_file}" | tr -d ' ')"
    expected_count="$(awk -v expected="${expected_status}" '$1 == expected { count++ } END { print count + 0 }' "${output_file}")"
    awk '{ print $2 }' "${output_file}" > "${latency_file}"
    sort -n "${latency_file}" > "${sorted_latency_file}"

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

    echo "| ${label} | ${REQUESTS} | ${average} | ${p95} | ${p99} | ${error_rate} | ${note} |" >> "${SUMMARY_FILE}"
}

write_redis_probe()
{
    cat > "${REDIS_PROBE}" <<'PY'
#!/usr/bin/env python3
import argparse
import socket
import time


def encode_command(parts):
    out = [f"*{len(parts)}\r\n".encode()]
    for part in parts:
        if isinstance(part, str):
            data = part.encode()
        else:
            data = part
        out.append(f"${len(data)}\r\n".encode())
        out.append(data)
        out.append(b"\r\n")
    return b"".join(out)


def read_line(sock):
    data = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            raise RuntimeError("connection closed")
        data.extend(chunk)
        if data.endswith(b"\r\n"):
            return bytes(data[:-2])


def read_reply(sock):
    prefix = sock.recv(1)
    if not prefix:
        raise RuntimeError("empty reply")

    if prefix == b"+":
        return read_line(sock).decode()
    if prefix == b"-":
        raise RuntimeError(read_line(sock).decode())
    if prefix == b":":
        return int(read_line(sock))
    if prefix == b"$":
        length = int(read_line(sock))
        if length == -1:
            return None
        data = bytearray()
        while len(data) < length + 2:
            chunk = sock.recv(length + 2 - len(data))
            if not chunk:
                raise RuntimeError("bulk reply closed")
            data.extend(chunk)
        return bytes(data[:length]).decode()

    raise RuntimeError(f"unsupported reply prefix {prefix!r}")


def connect(host, port, timeout):
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def command_for_mode(mode, key):
    if mode == "ping":
        return ["PING"]
    if mode.startswith("get"):
        return ["GET", key]
    raise RuntimeError(f"unsupported mode {mode}")


def is_expected(mode, reply, expected):
    if mode == "ping":
        return reply == "PONG"
    if mode.endswith("hit"):
        return reply == expected
    if mode.endswith("miss"):
        return reply is None
    return False


def run_once(host, port, timeout, mode, key, expected):
    start = time.perf_counter()
    try:
        with connect(host, port, timeout) as sock:
            sock.sendall(encode_command(command_for_mode(mode, key)))
            reply = read_reply(sock)
        ok = is_expected(mode, reply, expected)
    except Exception:
        ok = False
    elapsed = time.perf_counter() - start
    return ok, elapsed


def run_reuse(host, port, timeout, mode, key, expected, requests):
    try:
        sock = connect(host, port, timeout)
    except Exception:
        for _ in range(requests):
            print("fail 0")
        return

    with sock:
        for _ in range(requests):
            start = time.perf_counter()
            try:
                sock.sendall(encode_command(command_for_mode(mode, key)))
                reply = read_reply(sock)
                ok = is_expected(mode, reply, expected)
            except Exception:
                ok = False
            elapsed = time.perf_counter() - start
            print(("ok" if ok else "fail") + f" {elapsed:.6f}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--requests", required=True, type=int)
    parser.add_argument("--mode", required=True, choices=[
        "ping", "get-hit", "get-miss", "get-hit-reuse", "get-miss-reuse"
    ])
    parser.add_argument("--key", default="")
    parser.add_argument("--expected", default="")
    parser.add_argument("--timeout", default=2.0, type=float)
    args = parser.parse_args()

    if args.mode.endswith("reuse"):
        base_mode = args.mode[:-6]
        run_reuse(args.host, args.port, args.timeout, base_mode, args.key, args.expected, args.requests)
        return

    for _ in range(args.requests):
        ok, elapsed = run_once(args.host, args.port, args.timeout, args.mode, args.key, args.expected)
        print(("ok" if ok else "fail") + f" {elapsed:.6f}")


if __name__ == "__main__":
    main()
PY
    chmod +x "${REDIS_PROBE}"
}

run_redis_probe()
{
    local label="$1"
    local mode="$2"
    local key="$3"
    local expected="$4"
    local output_file="${ARTIFACT_DIR}/${mode}.txt"

    "${REDIS_PROBE}" \
        --host "${REDIS_HOST}" \
        --port "${REDIS_PORT}" \
        --requests "${REQUESTS}" \
        --mode "${mode}" \
        --key "${key}" \
        --expected "${expected}" \
        > "${output_file}"

    append_result_row "${label}" "ok" "${output_file}" "direct Redis via Python probe"
}

create_short_link()
{
    local body_file="${ARTIFACT_DIR}/create.body"
    local status

    status="$(curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -o "${body_file}" -w "%{http_code}" \
        -X POST "${BASE_URL}/api/short-links" \
        -b "${COOKIE_JAR}" \
        -H "Content-Type: application/json" \
        -d "{\"url\":\"${ORIGINAL_URL}\"}")"
    expect_eq "${status}" "201" "create short link status"

    REDIRECT_CODE="$(sed -n 's/.*"code":"\([^"]*\)".*/\1/p' "${body_file}")"
    [[ -n "${REDIRECT_CODE}" ]] || fail "create response did not contain code"
}

warm_redirect_cache()
{
    local body_file="${ARTIFACT_DIR}/warm.body"
    local header_file="${ARTIFACT_DIR}/warm.headers"
    local status

    status="$(curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -D "${header_file}" -o "${body_file}" -w "%{http_code}" \
        "${BASE_URL}/s/${REDIRECT_CODE}")"
    expect_eq "${status}" "302" "warm redirect status"
    expect_contains "$(tr -d '\r' < "${header_file}")" "Location: ${ORIGINAL_URL}" "warm redirect Location"
}

measure_http_path()
{
    local label="$1"
    local path="$2"
    local expected_status="$3"
    local output_name="$4"
    local output_file="${ARTIFACT_DIR}/${output_name}.txt"
    local body_file="${ARTIFACT_DIR}/${output_name}.body"
    local header_file="${ARTIFACT_DIR}/${output_name}.headers"
    local status time_total
    local i

    : > "${output_file}"
    for i in $(seq 1 "${REQUESTS}"); do
        line="$(curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -D "${header_file}" -o "${body_file}" -w "%{http_code} %{time_total}" \
            "${BASE_URL}${path}" 2>/dev/null || printf "000 0")"
        status="$(awk '{ print $1 }' <<< "${line}")"
        time_total="$(awk '{ print $2 }' <<< "${line}")"
        printf "%s %s\n" "${status}" "${time_total}" >> "${output_file}"
    done

    append_result_row "${label}" "${expected_status}" "${output_file}" "HTTP curl sequential"
}

measure_http_redis_miss()
{
    local output_file="${ARTIFACT_DIR}/http-redis-miss.txt"
    local body_file="${ARTIFACT_DIR}/http-redis-miss.body"
    local header_file="${ARTIFACT_DIR}/http-redis-miss.headers"
    local status time_total line
    local i

    : > "${output_file}"
    for i in $(seq 1 "${REQUESTS}"); do
        redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" DEL "${REDIS_KEY_PREFIX}${REDIRECT_CODE}" >/dev/null
        line="$(curl -sS --max-time "${CURL_MAX_TIME_SECONDS}" -D "${header_file}" -o "${body_file}" -w "%{http_code} %{time_total}" \
            "${BASE_URL}/s/${REDIRECT_CODE}" 2>/dev/null || printf "000 0")"
        status="$(awk '{ print $1 }' <<< "${line}")"
        time_total="$(awk '{ print $2 }' <<< "${line}")"
        printf "%s %s\n" "${status}" "${time_total}" >> "${output_file}"
    done

    append_result_row "HTTP Redis miss with MySQL backfill" "302" "${output_file}" "Redis key deleted before each request"
    warm_redirect_cache
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
command -v redis-cli >/dev/null 2>&1 || fail "redis-cli is required"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"

mysql_cmd -e "SELECT 1" >/dev/null 2>&1 ||
    fail "MySQL is not reachable at ${MYSQL_HOST}:${MYSQL_PORT}; start Compose mysql dependency first"
redis_ping="$(redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" PING 2>/dev/null || true)"
expect_eq "${redis_ping}" "PONG" "Redis should be reachable at ${REDIS_HOST}:${REDIS_PORT}"

mkdir -p "${ARTIFACT_DIR}"
trap cleanup EXIT
write_redis_probe

cat > "${SUMMARY_FILE}" <<EOF
# Redis cache diagnostic

- commit: $(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)
- requests per scenario: ${REQUESTS}
- port: ${PORT}
- server.thread_num: ${THREAD_NUM}
- mysql.pool_size: ${MYSQL_POOL_SIZE}
- artifact: ${ARTIFACT_DIR}
- redis: ${REDIS_HOST}:${REDIS_PORT}

| 场景 | 请求数 | 平均延迟 | P95 | P99 | 错误率 | 备注 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
EOF

echo "Artifacts: ${ARTIFACT_DIR}"
echo "Summary: ${SUMMARY_FILE}"

start_server "true" "mysql-redis"
create_short_link
warm_redirect_cache
REDIS_KEY="${REDIS_KEY_PREFIX}${REDIRECT_CODE}"
REDIS_VALUE="$(redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" GET "${REDIS_KEY}")"
expect_contains "${REDIS_VALUE}" "v2|" "Redis cache value version"
expect_contains "${REDIS_VALUE}" "|${ORIGINAL_URL}" "Redis cache value URL"
run_redis_probe "Redis PING new connection" "ping" "" ""
run_redis_probe "Redis GET hit new connection" "get-hit" "${REDIS_KEY}" "${REDIS_VALUE}"
run_redis_probe "Redis GET hit reused connection" "get-hit-reuse" "${REDIS_KEY}" "${REDIS_VALUE}"
run_redis_probe "Redis GET miss new connection" "get-miss" "${REDIS_KEY_PREFIX}missing-${RUN_ID}" ""
run_redis_probe "Redis GET miss reused connection" "get-miss-reuse" "${REDIS_KEY_PREFIX}missing-${RUN_ID}" ""
measure_http_path "HTTP Redis hit" "/s/${REDIRECT_CODE}" "302" "http-redis-hit"
measure_http_redis_miss
measure_http_path "HTTP mysql-redis missing-code" "/s/notfound-redis-diagnostic-${RUN_ID}" "404" "http-redis-missing-code"

start_server "false" "mysql-only"
measure_http_path "HTTP MySQL redirect" "/s/${REDIRECT_CODE}" "302" "http-mysql-redirect"
measure_http_path "HTTP MySQL missing-code" "/s/notfound-mysql-diagnostic-${RUN_ID}" "404" "http-mysql-missing-code"

echo "Redis cache diagnostic complete."
echo "Artifacts preserved at: ${ARTIFACT_DIR}"
