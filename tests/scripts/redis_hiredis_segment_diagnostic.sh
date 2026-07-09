#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REQUESTS="${HAOHTTP_REDIS_SEGMENT_REQUESTS:-100}"
REDIS_HOST="${HAOHTTP_REDIS_HOST:-docker.orb.internal}"
REDIS_PORT="${HAOHTTP_REDIS_PORT:-16379}"
REDIS_DATABASE="${HAOHTTP_REDIS_DATABASE:-0}"
REDIS_KEY_PREFIX="${HAOHTTP_REDIS_KEY_PREFIX:-shortlink:}"
REDIS_TTL_SECONDS="${HAOHTTP_REDIS_TTL_SECONDS:-3600}"
CONNECT_TIMEOUT_MS="${HAOHTTP_REDIS_SEGMENT_CONNECT_TIMEOUT_MS:-1000}"
ARTIFACT_DIR="${HAOHTTP_REDIS_SEGMENT_ARTIFACT_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/haohttp-redis-segment-diagnostic.XXXXXX")}"
SUMMARY_FILE="${ARTIFACT_DIR}/summary.md"
PROBE_CPP="${ARTIFACT_DIR}/hiredis_segment_probe.cpp"
PROBE_BIN="${ARTIFACT_DIR}/hiredis_segment_probe"
RAW_FILE="${ARTIFACT_DIR}/hiredis_segments.tsv"
RUN_ID="$(date +%s)-${RANDOM}"
HIT_KEY="${REDIS_KEY_PREFIX}segment-hit-${RUN_ID}"
MISS_KEY="${REDIS_KEY_PREFIX}segment-miss-${RUN_ID}"
SET_KEY="${REDIS_KEY_PREFIX}segment-set-${RUN_ID}"
HIT_VALUE="https://example.com/redis-segment-hit-${RUN_ID}"
SET_VALUE="https://example.com/redis-segment-set-${RUN_ID}"

usage()
{
    cat <<EOF
Usage: bash tests/scripts/redis_hiredis_segment_diagnostic.sh

Runs v1.4.5.2 Redis hiredis segment diagnostics. The script compiles a
temporary C++ probe and measures resolve, redisConnectWithTimeout, SELECT,
GET/SETEX/PING, and redisFree durations separately. It also runs TCP_NODELAY
variants to check whether small Redis command packets are delayed by TCP.

Common environment variables:
  HAOHTTP_REDIS_SEGMENT_REQUESTS            default: ${REQUESTS}
  HAOHTTP_REDIS_SEGMENT_CONNECT_TIMEOUT_MS  default: ${CONNECT_TIMEOUT_MS}
  HAOHTTP_REDIS_SEGMENT_ARTIFACT_DIR        default: auto-created under /tmp

Redis variables:
  HAOHTTP_REDIS_HOST        default: ${REDIS_HOST}
  HAOHTTP_REDIS_PORT        default: ${REDIS_PORT}
  HAOHTTP_REDIS_DATABASE    default: ${REDIS_DATABASE}
  HAOHTTP_REDIS_KEY_PREFIX  default: ${REDIS_KEY_PREFIX}
  HAOHTTP_REDIS_TTL_SECONDS default: ${REDIS_TTL_SECONDS}
EOF
}

fail()
{
    echo "FAIL: $*" >&2
    echo "Artifacts preserved at: ${ARTIFACT_DIR}" >&2
    exit 1
}

write_probe()
{
    cat > "${PROBE_CPP}" <<'CPP'
#include <hiredis/hiredis.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

struct Config
{
    std::string host = "127.0.0.1";
    int port = 6379;
    int database = 0;
    int ttlSeconds = 3600;
    int requests = 100;
    int connectTimeoutMs = 1000;
    std::string hitKey;
    std::string missKey;
    std::string setKey;
    std::string hitValue;
    std::string setValue;
};

struct ReplyDeleter
{
    void operator()(redisReply* reply) const
    {
        if (reply)
        {
            freeReplyObject(reply);
        }
    }
};

using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

double elapsedSeconds(const Clock::time_point& start, const Clock::time_point& end)
{
    return std::chrono::duration<double>(end - start).count();
}

timeval timeoutFromMs(int milliseconds)
{
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return timeout;
}

bool parseInt(const std::string& value, int* out)
{
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!end || *end != '\0')
    {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

Config parseArgs(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc)
        {
            config.host = argv[++i];
        }
        else if (arg == "--port" && i + 1 < argc)
        {
            parseInt(argv[++i], &config.port);
        }
        else if (arg == "--database" && i + 1 < argc)
        {
            parseInt(argv[++i], &config.database);
        }
        else if (arg == "--ttl-seconds" && i + 1 < argc)
        {
            parseInt(argv[++i], &config.ttlSeconds);
        }
        else if (arg == "--requests" && i + 1 < argc)
        {
            parseInt(argv[++i], &config.requests);
        }
        else if (arg == "--connect-timeout-ms" && i + 1 < argc)
        {
            parseInt(argv[++i], &config.connectTimeoutMs);
        }
        else if (arg == "--hit-key" && i + 1 < argc)
        {
            config.hitKey = argv[++i];
        }
        else if (arg == "--miss-key" && i + 1 < argc)
        {
            config.missKey = argv[++i];
        }
        else if (arg == "--set-key" && i + 1 < argc)
        {
            config.setKey = argv[++i];
        }
        else if (arg == "--hit-value" && i + 1 < argc)
        {
            config.hitValue = argv[++i];
        }
        else if (arg == "--set-value" && i + 1 < argc)
        {
            config.setValue = argv[++i];
        }
    }
    return config;
}

ReplyPtr command(redisContext* context, const char* format)
{
    return ReplyPtr(static_cast<redisReply*>(redisCommand(context, format)));
}

bool selectDatabase(redisContext* context, int database)
{
    if (database <= 0)
    {
        return true;
    }

    std::ostringstream commandText;
    commandText << "SELECT " << database;
    ReplyPtr reply(command(context, commandText.str().c_str()));
    return reply && reply->type != REDIS_REPLY_ERROR;
}

redisContext* connectRedis(const Config& config, double* connectSeconds)
{
    const timeval timeout = timeoutFromMs(config.connectTimeoutMs);
    const auto start = Clock::now();
    redisContext* context = redisConnectWithTimeout(config.host.c_str(), config.port, timeout);
    const auto end = Clock::now();
    *connectSeconds = elapsedSeconds(start, end);
    return context;
}

bool enableTcpNoDelay(redisContext* context)
{
    int enabled = 1;
    return setsockopt(context->fd,
                      IPPROTO_TCP,
                      TCP_NODELAY,
                      &enabled,
                      sizeof(enabled)) == 0;
}

bool setupKeys(const Config& config)
{
    double ignored = 0.0;
    redisContext* context = connectRedis(config, &ignored);
    if (!context || context->err)
    {
        if (context)
        {
            redisFree(context);
        }
        return false;
    }

    if (!selectDatabase(context, config.database))
    {
        redisFree(context);
        return false;
    }

    ReplyPtr setReply(static_cast<redisReply*>(redisCommand(context,
                                                           "SETEX %b %d %b",
                                                           config.hitKey.data(),
                                                           config.hitKey.size(),
                                                           config.ttlSeconds,
                                                           config.hitValue.data(),
                                                           config.hitValue.size())));
    ReplyPtr delMiss(static_cast<redisReply*>(redisCommand(context,
                                                           "DEL %b",
                                                           config.missKey.data(),
                                                           config.missKey.size())));
    ReplyPtr delSet(static_cast<redisReply*>(redisCommand(context,
                                                          "DEL %b",
                                                          config.setKey.data(),
                                                          config.setKey.size())));
    redisFree(context);
    return setReply && setReply->type == REDIS_REPLY_STATUS && delMiss && delSet;
}

void cleanupKeys(const Config& config)
{
    double ignored = 0.0;
    redisContext* context = connectRedis(config, &ignored);
    if (!context || context->err)
    {
        if (context)
        {
            redisFree(context);
        }
        return;
    }

    if (selectDatabase(context, config.database))
    {
        ReplyPtr delHit(static_cast<redisReply*>(redisCommand(context,
                                                              "DEL %b",
                                                              config.hitKey.data(),
                                                              config.hitKey.size())));
        ReplyPtr delMiss(static_cast<redisReply*>(redisCommand(context,
                                                               "DEL %b",
                                                               config.missKey.data(),
                                                               config.missKey.size())));
        ReplyPtr delSet(static_cast<redisReply*>(redisCommand(context,
                                                              "DEL %b",
                                                              config.setKey.data(),
                                                              config.setKey.size())));
        (void)delHit;
        (void)delMiss;
        (void)delSet;
    }

    redisFree(context);
}

void printRow(const std::string& scenario,
              bool ok,
              double total,
              double resolve,
              double connect,
              double select,
              double command,
              double free)
{
    std::cout << scenario << '\t'
              << (ok ? "ok" : "error") << '\t'
              << total << '\t'
              << resolve << '\t'
              << connect << '\t'
              << select << '\t'
              << command << '\t'
              << free << '\n';
}

void runResolveOnly(const Config& config)
{
    const std::string service = std::to_string(config.port);
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;

    for (int i = 0; i < config.requests; ++i)
    {
        addrinfo* result = nullptr;
        const auto start = Clock::now();
        const int rc = getaddrinfo(config.host.c_str(), service.c_str(), &hints, &result);
        const auto end = Clock::now();
        if (result)
        {
            freeaddrinfo(result);
        }
        const double seconds = elapsedSeconds(start, end);
        printRow("resolve_only", rc == 0, seconds, seconds, 0.0, 0.0, 0.0, 0.0);
    }
}

enum class NewConnectionCommand
{
    ConnectFree,
    Ping,
    GetHit,
    GetMiss,
    Setex
};

void runNewConnectionScenario(const Config& config,
                              const std::string& scenario,
                              NewConnectionCommand operation,
                              bool tcpNoDelay)
{
    for (int i = 0; i < config.requests; ++i)
    {
        const auto totalStart = Clock::now();
        double connectSeconds = 0.0;
        double selectSeconds = 0.0;
        double commandSeconds = 0.0;
        double freeSeconds = 0.0;
        bool ok = true;

        redisContext* context = connectRedis(config, &connectSeconds);
        if (!context || context->err)
        {
            ok = false;
        }

        if (ok && tcpNoDelay)
        {
            ok = enableTcpNoDelay(context);
        }

        if (ok && config.database > 0)
        {
            const auto selectStart = Clock::now();
            ok = selectDatabase(context, config.database);
            const auto selectEnd = Clock::now();
            selectSeconds = elapsedSeconds(selectStart, selectEnd);
        }

        if (ok && operation != NewConnectionCommand::ConnectFree)
        {
            const auto commandStart = Clock::now();
            ReplyPtr reply;
            if (operation == NewConnectionCommand::Ping)
            {
                reply = command(context, "PING");
                ok = reply && reply->type == REDIS_REPLY_STATUS;
            }
            else if (operation == NewConnectionCommand::GetHit)
            {
                reply.reset(static_cast<redisReply*>(redisCommand(context,
                                                                  "GET %b",
                                                                  config.hitKey.data(),
                                                                  config.hitKey.size())));
                ok = reply && reply->type == REDIS_REPLY_STRING;
            }
            else if (operation == NewConnectionCommand::GetMiss)
            {
                reply.reset(static_cast<redisReply*>(redisCommand(context,
                                                                  "GET %b",
                                                                  config.missKey.data(),
                                                                  config.missKey.size())));
                ok = reply && reply->type == REDIS_REPLY_NIL;
            }
            else if (operation == NewConnectionCommand::Setex)
            {
                reply.reset(static_cast<redisReply*>(redisCommand(context,
                                                                  "SETEX %b %d %b",
                                                                  config.setKey.data(),
                                                                  config.setKey.size(),
                                                                  config.ttlSeconds,
                                                                  config.setValue.data(),
                                                                  config.setValue.size())));
                ok = reply && reply->type == REDIS_REPLY_STATUS;
            }
            const auto commandEnd = Clock::now();
            commandSeconds = elapsedSeconds(commandStart, commandEnd);
        }

        if (context)
        {
            const auto freeStart = Clock::now();
            redisFree(context);
            const auto freeEnd = Clock::now();
            freeSeconds = elapsedSeconds(freeStart, freeEnd);
        }

        const auto totalEnd = Clock::now();
        printRow(scenario,
                 ok,
                 elapsedSeconds(totalStart, totalEnd),
                 0.0,
                 connectSeconds,
                 selectSeconds,
                 commandSeconds,
                 freeSeconds);
    }
}

void runReusedConnectionScenario(const Config& config,
                                 const std::string& scenario,
                                 NewConnectionCommand operation,
                                 bool tcpNoDelay)
{
    double ignored = 0.0;
    redisContext* context = connectRedis(config, &ignored);
    if (!context || context->err || (tcpNoDelay && !enableTcpNoDelay(context)) ||
        !selectDatabase(context, config.database))
    {
        if (context)
        {
            redisFree(context);
        }
        for (int i = 0; i < config.requests; ++i)
        {
            printRow(scenario, false, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        }
        return;
    }

    for (int i = 0; i < config.requests; ++i)
    {
        const auto commandStart = Clock::now();
        bool ok = true;
        ReplyPtr reply;
        if (operation == NewConnectionCommand::GetHit)
        {
            reply.reset(static_cast<redisReply*>(redisCommand(context,
                                                              "GET %b",
                                                              config.hitKey.data(),
                                                              config.hitKey.size())));
            ok = reply && reply->type == REDIS_REPLY_STRING;
        }
        else if (operation == NewConnectionCommand::GetMiss)
        {
            reply.reset(static_cast<redisReply*>(redisCommand(context,
                                                              "GET %b",
                                                              config.missKey.data(),
                                                              config.missKey.size())));
            ok = reply && reply->type == REDIS_REPLY_NIL;
        }
        else if (operation == NewConnectionCommand::Setex)
        {
            reply.reset(static_cast<redisReply*>(redisCommand(context,
                                                              "SETEX %b %d %b",
                                                              config.setKey.data(),
                                                              config.setKey.size(),
                                                              config.ttlSeconds,
                                                              config.setValue.data(),
                                                              config.setValue.size())));
            ok = reply && reply->type == REDIS_REPLY_STATUS;
        }
        const auto commandEnd = Clock::now();
        const double seconds = elapsedSeconds(commandStart, commandEnd);
        printRow(scenario, ok, seconds, 0.0, 0.0, 0.0, seconds, 0.0);
    }

    redisFree(context);
}

} // namespace

int main(int argc, char** argv)
{
    const Config config = parseArgs(argc, argv);
    if (config.hitKey.empty() || config.missKey.empty() || config.setKey.empty())
    {
        std::cerr << "missing key arguments\n";
        return 2;
    }

    if (!setupKeys(config))
    {
        std::cerr << "failed to set up redis diagnostic keys\n";
        return 1;
    }

    std::cout << "scenario\tstatus\ttotal_s\tresolve_s\tconnect_s\tselect_s\tcommand_s\tfree_s\n";
    runResolveOnly(config);
    runNewConnectionScenario(config, "connect_free", NewConnectionCommand::ConnectFree, false);
    runNewConnectionScenario(config, "ping_new_connection", NewConnectionCommand::Ping, false);
    runNewConnectionScenario(config, "get_hit_new_connection", NewConnectionCommand::GetHit, false);
    runNewConnectionScenario(config, "get_miss_new_connection", NewConnectionCommand::GetMiss, false);
    runNewConnectionScenario(config, "setex_new_connection", NewConnectionCommand::Setex, false);
    runReusedConnectionScenario(config, "get_hit_reused_connection", NewConnectionCommand::GetHit, false);
    runReusedConnectionScenario(config, "get_miss_reused_connection", NewConnectionCommand::GetMiss, false);
    runReusedConnectionScenario(config, "setex_reused_connection", NewConnectionCommand::Setex, false);
    runNewConnectionScenario(config,
                             "ping_new_connection_tcp_nodelay",
                             NewConnectionCommand::Ping,
                             true);
    runNewConnectionScenario(config,
                             "get_hit_new_connection_tcp_nodelay",
                             NewConnectionCommand::GetHit,
                             true);
    runNewConnectionScenario(config,
                             "get_miss_new_connection_tcp_nodelay",
                             NewConnectionCommand::GetMiss,
                             true);
    runNewConnectionScenario(config,
                             "setex_new_connection_tcp_nodelay",
                             NewConnectionCommand::Setex,
                             true);
    runReusedConnectionScenario(config,
                                "get_hit_reused_connection_tcp_nodelay",
                                NewConnectionCommand::GetHit,
                                true);
    runReusedConnectionScenario(config,
                                "get_miss_reused_connection_tcp_nodelay",
                                NewConnectionCommand::GetMiss,
                                true);
    runReusedConnectionScenario(config,
                                "setex_reused_connection_tcp_nodelay",
                                NewConnectionCommand::Setex,
                                true);

    cleanupKeys(config);
    return 0;
}
CPP
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

average_column()
{
    local file="$1"
    local column="$2"

    awk -v column="${column}" 'NR > 1 { sum += $column; count++ } END {
        if (count > 0) printf "%.6fs", sum / count;
        else print "unknown";
    }' "${file}"
}

append_scenario_row()
{
    local scenario="$1"
    local scenario_file="${ARTIFACT_DIR}/${scenario}.tsv"
    local total_latency_file="${ARTIFACT_DIR}/${scenario}.total.sorted"
    local total ok_count error_rate average_total p95 p99 average_resolve average_connect average_select average_command average_free

    awk -F '\t' -v scenario="${scenario}" 'NR == 1 || $1 == scenario { print }' "${RAW_FILE}" > "${scenario_file}"
    total="$(( $(wc -l < "${scenario_file}" | tr -d ' ') - 1 ))"
    ok_count="$(awk -F '\t' 'NR > 1 && $2 == "ok" { count++ } END { print count + 0 }' "${scenario_file}")"
    awk -F '\t' 'NR > 1 { print $3 }' "${scenario_file}" | sort -g > "${total_latency_file}"

    average_total="$(average_column "${scenario_file}" 3)"
    p95="$(percentile_from_sorted_file "${total_latency_file}" "${total}" 95)"
    p99="$(percentile_from_sorted_file "${total_latency_file}" "${total}" 99)"
    average_resolve="$(average_column "${scenario_file}" 4)"
    average_connect="$(average_column "${scenario_file}" 5)"
    average_select="$(average_column "${scenario_file}" 6)"
    average_command="$(average_column "${scenario_file}" 7)"
    average_free="$(average_column "${scenario_file}" 8)"

    if [[ "${p95}" != "unknown" ]]; then
        p95="${p95}s"
    fi
    if [[ "${p99}" != "unknown" ]]; then
        p99="${p99}s"
    fi

    if [[ "${total}" -gt 0 ]]; then
        error_rate="$(awk -v total="${total}" -v ok="${ok_count}" 'BEGIN { printf "%.2f%%", ((total - ok) * 100.0 / total) }')"
    else
        error_rate="unknown"
    fi

    echo "| ${scenario} | ${total} | ${average_total} | ${p95} | ${p99} | ${average_resolve} | ${average_connect} | ${average_select} | ${average_command} | ${average_free} | ${error_rate} |" >> "${SUMMARY_FILE}"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

mkdir -p "${ARTIFACT_DIR}"

{
    echo "# Redis hiredis segment diagnostic"
    echo
    echo "- artifact: \`${ARTIFACT_DIR}\`"
    echo "- requests: ${REQUESTS}"
    echo "- redis: \`${REDIS_HOST}:${REDIS_PORT}\`"
    echo "- redis database: ${REDIS_DATABASE}"
    echo "- connect timeout: ${CONNECT_TIMEOUT_MS}ms"
    echo "- hit key: \`${HIT_KEY}\`"
    echo "- miss key: \`${MISS_KEY}\`"
    echo "- set key: \`${SET_KEY}\`"
    echo
    echo "## Host resolution"
    echo
    echo '```text'
} > "${SUMMARY_FILE}"

if command -v getent >/dev/null 2>&1; then
    getent hosts "${REDIS_HOST}" >> "${SUMMARY_FILE}" 2>&1 || true
else
    echo "getent is not available" >> "${SUMMARY_FILE}"
fi

{
    echo '```'
    echo
} >> "${SUMMARY_FILE}"

write_probe

if ! command -v c++ >/dev/null 2>&1; then
    fail "c++ compiler is not available"
fi

(
    cd "${ROOT_DIR}"
    c++ -std=c++17 -O2 "${PROBE_CPP}" -o "${PROBE_BIN}" -lhiredis
) || fail "failed to compile hiredis segment probe"

"${PROBE_BIN}" \
    --host "${REDIS_HOST}" \
    --port "${REDIS_PORT}" \
    --database "${REDIS_DATABASE}" \
    --ttl-seconds "${REDIS_TTL_SECONDS}" \
    --requests "${REQUESTS}" \
    --connect-timeout-ms "${CONNECT_TIMEOUT_MS}" \
    --hit-key "${HIT_KEY}" \
    --miss-key "${MISS_KEY}" \
    --set-key "${SET_KEY}" \
    --hit-value "${HIT_VALUE}" \
    --set-value "${SET_VALUE}" \
    > "${RAW_FILE}" || fail "hiredis segment probe failed"

{
    echo "## Results"
    echo
    echo "| Scenario | Requests | Avg total | P95 total | P99 total | Avg resolve | Avg connect | Avg SELECT | Avg command | Avg free | Error rate |"
    echo "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"
} >> "${SUMMARY_FILE}"

append_scenario_row "resolve_only"
append_scenario_row "connect_free"
append_scenario_row "ping_new_connection"
append_scenario_row "get_hit_new_connection"
append_scenario_row "get_miss_new_connection"
append_scenario_row "setex_new_connection"
append_scenario_row "get_hit_reused_connection"
append_scenario_row "get_miss_reused_connection"
append_scenario_row "setex_reused_connection"
append_scenario_row "ping_new_connection_tcp_nodelay"
append_scenario_row "get_hit_new_connection_tcp_nodelay"
append_scenario_row "get_miss_new_connection_tcp_nodelay"
append_scenario_row "setex_new_connection_tcp_nodelay"
append_scenario_row "get_hit_reused_connection_tcp_nodelay"
append_scenario_row "get_miss_reused_connection_tcp_nodelay"
append_scenario_row "setex_reused_connection_tcp_nodelay"

{
    echo
    echo "## Raw files"
    echo
    echo "- \`${RAW_FILE}\`: raw per-iteration segment timings."
    echo "- \`${PROBE_CPP}\`: temporary C++ hiredis probe source."
    echo "- \`${PROBE_BIN}\`: compiled temporary probe binary."
} >> "${SUMMARY_FILE}"

echo "Redis hiredis segment diagnostic complete."
echo "Summary: ${SUMMARY_FILE}"
