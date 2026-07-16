#include "http/HttpServer.h"
#include "shortlink/MemoryShortLinkRepository.h"
#include "shortlink/MySqlShortLinkRepository.h"
#include "shortlink/RateLimiter.h"
#include "shortlink/RedisCachedShortLinkRepository.h"
#include "shortlink/RedisFixedWindowRateLimiter.h"
#include "shortlink/RedisShortLinkCache.h"
#include "shortlink/ShortLinkService.h"
#include "shortlink/ShortLinkMetrics.h"
#include "utils/Config.h"
#include "utils/JsonUtil.h"
#include "utils/db/DbConnectionPool.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <muduo/base/Logging.h>

namespace
{

struct ServerConfig
{
    std::string name = "HaoShortLink";
    int port = 8080;
    int threadNum = 4;
    bool metricsEnabled = true;
    std::string storageType = "memory";
    std::string mysqlHost = "tcp://127.0.0.1:3306";
    std::string mysqlUser = "root";
    std::string mysqlPassword;
    std::string mysqlDatabase = "hao_shortlink";
    int mysqlPoolSize = 4;
    bool redisEnabled = false;
    std::string redisHost = "127.0.0.1";
    int redisPort = 6379;
    int redisDatabase = 0;
    int redisTtlSeconds = 3600;
    std::string redisKeyPrefix = "shortlink:";
    bool rateLimitEnabled = false;
    int rateLimitRequests = 100;
    int rateLimitWindowSeconds = 60;
    std::string rateLimitKeyPrefix = "rate-limit:create:";
};

ServerConfig loadServerConfig(const std::string& configPath)
{
    ServerConfig serverConfig;

    http::utils::Config config;
    if (!configPath.empty())
    {
        if (!config.loadFromFile(configPath))
        {
            std::cerr << "Failed to load config file: " << configPath
                      << ". Using default server config." << std::endl;
            return serverConfig;
        }

        serverConfig.name = config.getString("server.name", serverConfig.name);
        serverConfig.port = config.getInt("server.port", serverConfig.port);
        serverConfig.threadNum = config.getInt("server.thread_num", serverConfig.threadNum);
        serverConfig.metricsEnabled = config.getBool("metrics.enabled", serverConfig.metricsEnabled);
        serverConfig.storageType = config.getString("storage.type", serverConfig.storageType);
        serverConfig.mysqlHost = config.getString("mysql.host", serverConfig.mysqlHost);
        serverConfig.mysqlUser = config.getString("mysql.user", serverConfig.mysqlUser);
        serverConfig.mysqlPassword = config.getString("mysql.password", serverConfig.mysqlPassword);
        serverConfig.mysqlDatabase = config.getString("mysql.database", serverConfig.mysqlDatabase);
        serverConfig.mysqlPoolSize = config.getInt("mysql.pool_size", serverConfig.mysqlPoolSize);
        serverConfig.redisEnabled = config.getBool("redis.enabled", serverConfig.redisEnabled);
        serverConfig.redisHost = config.getString("redis.host", serverConfig.redisHost);
        serverConfig.redisPort = config.getInt("redis.port", serverConfig.redisPort);
        serverConfig.redisDatabase = config.getInt("redis.database", serverConfig.redisDatabase);
        serverConfig.redisTtlSeconds = config.getInt("redis.ttl_seconds", serverConfig.redisTtlSeconds);
        serverConfig.redisKeyPrefix = config.getString("redis.key_prefix", serverConfig.redisKeyPrefix);
        serverConfig.rateLimitEnabled = config.getBool("rate_limit.enabled", serverConfig.rateLimitEnabled);
        serverConfig.rateLimitRequests = config.getInt("rate_limit.requests", serverConfig.rateLimitRequests);
        serverConfig.rateLimitWindowSeconds =
            config.getInt("rate_limit.window_seconds", serverConfig.rateLimitWindowSeconds);
        serverConfig.rateLimitKeyPrefix =
            config.getString("rate_limit.key_prefix", serverConfig.rateLimitKeyPrefix);
    }

    if (serverConfig.port <= 0 || serverConfig.port > 65535)
    {
        std::cerr << "Invalid server.port. Using default port 8080." << std::endl;
        serverConfig.port = 8080;
    }

    if (serverConfig.threadNum <= 0)
    {
        std::cerr << "Invalid server.thread_num. Using default thread_num 4." << std::endl;
        serverConfig.threadNum = 4;
    }

    if (serverConfig.storageType != "memory" && serverConfig.storageType != "mysql")
    {
        std::cerr << "Invalid storage.type. Using memory storage." << std::endl;
        serverConfig.storageType = "memory";
    }

    if (serverConfig.mysqlPoolSize <= 0)
    {
        std::cerr << "Invalid mysql.pool_size. Using default pool_size 4." << std::endl;
        serverConfig.mysqlPoolSize = 4;
    }

    if (serverConfig.redisPort <= 0 || serverConfig.redisPort > 65535)
    {
        std::cerr << "Invalid redis.port. Using default port 6379." << std::endl;
        serverConfig.redisPort = 6379;
    }

    if (serverConfig.redisDatabase < 0)
    {
        std::cerr << "Invalid redis.database. Using default database 0." << std::endl;
        serverConfig.redisDatabase = 0;
    }

    if (serverConfig.redisTtlSeconds <= 0 || serverConfig.redisTtlSeconds > 86400)
    {
        std::cerr << "redis.ttl_seconds must be between 1 and 86400. "
                  << "Using default ttl_seconds 3600." << std::endl;
        serverConfig.redisTtlSeconds = 3600;
    }

    if (serverConfig.rateLimitRequests <= 0)
    {
        std::cerr << "Invalid rate_limit.requests. Disabling rate limiting." << std::endl;
        serverConfig.rateLimitEnabled = false;
    }

    if (serverConfig.rateLimitWindowSeconds <= 0)
    {
        std::cerr << "Invalid rate_limit.window_seconds. Disabling rate limiting." << std::endl;
        serverConfig.rateLimitEnabled = false;
    }

    if (serverConfig.rateLimitKeyPrefix.empty())
    {
        std::cerr << "Invalid rate_limit.key_prefix. Disabling rate limiting." << std::endl;
        serverConfig.rateLimitEnabled = false;
    }

    if (serverConfig.rateLimitEnabled &&
        serverConfig.rateLimitKeyPrefix == serverConfig.redisKeyPrefix)
    {
        std::cerr << "rate_limit.key_prefix must differ from redis.key_prefix. "
                  << "Disabling rate limiting." << std::endl;
        serverConfig.rateLimitEnabled = false;
    }

    return serverConfig;
}

void setJsonResponse(http::HttpResponse* resp,
                     http::HttpResponse::HttpStatusCode statusCode,
                     const std::string& statusMessage,
                     const std::string& body)
{
    resp->setStatusCode(statusCode);
    resp->setStatusMessage(statusMessage);
    resp->setJsonBody(body);
}

bool hasJsonContentType(const http::HttpRequest& req)
{
    const std::string contentType = req.getHeader("Content-Type");
    return contentType.find("application/json") != std::string::npos;
}

void skipWhitespace(const std::string& value, std::size_t* pos)
{
    while (*pos < value.size() && std::isspace(static_cast<unsigned char>(value[*pos])))
    {
        ++(*pos);
    }
}

bool consumeChar(const std::string& value, std::size_t* pos, char expected)
{
    skipWhitespace(value, pos);
    if (*pos >= value.size() || value[*pos] != expected)
    {
        return false;
    }

    ++(*pos);
    return true;
}

std::optional<std::string> parseJsonString(const std::string& value, std::size_t* pos)
{
    skipWhitespace(value, pos);
    if (*pos >= value.size() || value[*pos] != '"')
    {
        return std::nullopt;
    }

    ++(*pos);

    std::string parsed;
    while (*pos < value.size())
    {
        const char current = value[(*pos)++];
        if (current == '"')
        {
            return parsed;
        }

        if (current == '\\')
        {
            if (*pos >= value.size())
            {
                return std::nullopt;
            }

            const char escaped = value[(*pos)++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                parsed.push_back(escaped);
                break;
            case 'n':
                parsed.push_back('\n');
                break;
            case 'r':
                parsed.push_back('\r');
                break;
            case 't':
                parsed.push_back('\t');
                break;
            default:
                return std::nullopt;
            }
            continue;
        }

        parsed.push_back(current);
    }

    return std::nullopt;
}

struct JsonStringOrNull
{
    bool isNull { false };
    std::string value;
};

using SimpleJsonObject = std::unordered_map<std::string, JsonStringOrNull>;

std::optional<SimpleJsonObject> parseSimpleJsonObject(const std::string& body)
{
    std::size_t pos = 0;
    if (!consumeChar(body, &pos, '{'))
    {
        return std::nullopt;
    }

    SimpleJsonObject object;

    while (true)
    {
        skipWhitespace(body, &pos);
        if (pos < body.size() && body[pos] == '}')
        {
            ++pos;
            break;
        }

        const std::optional<std::string> key = parseJsonString(body, &pos);
        if (!key || !consumeChar(body, &pos, ':'))
        {
            return std::nullopt;
        }

        JsonStringOrNull value;
        skipWhitespace(body, &pos);
        if (body.compare(pos, 4, "null") == 0)
        {
            value.isNull = true;
            pos += 4;
        }
        else
        {
            const std::optional<std::string> parsed = parseJsonString(body, &pos);
            if (!parsed)
            {
                return std::nullopt;
            }
            value.value = *parsed;
        }

        if (object.find(*key) != object.end())
        {
            return std::nullopt;
        }
        object.emplace(*key, std::move(value));

        skipWhitespace(body, &pos);
        if (pos < body.size() && body[pos] == ',')
        {
            ++pos;
            skipWhitespace(body, &pos);
            if (pos >= body.size() || body[pos] == '}')
            {
                return std::nullopt;
            }
            continue;
        }
        if (pos < body.size() && body[pos] == '}')
        {
            ++pos;
            break;
        }

        return std::nullopt;
    }

    skipWhitespace(body, &pos);
    if (pos != body.size())
    {
        return std::nullopt;
    }
    return object;
}

struct CreateRequest
{
    std::string url;
    std::optional<std::int64_t> expiresAt;
};

std::optional<CreateRequest> parseCreateRequest(const std::string& body)
{
    const std::optional<SimpleJsonObject> object = parseSimpleJsonObject(body);
    if (!object || object->find("url") == object->end() || object->at("url").isNull ||
        object->size() > 2)
    {
        return std::nullopt;
    }

    CreateRequest request;
    request.url = object->at("url").value;
    for (const auto& item : *object)
    {
        if (item.first != "url" && item.first != "expires_at")
        {
            return std::nullopt;
        }
    }
    const auto expires = object->find("expires_at");
    if (expires != object->end() && !expires->second.isNull)
    {
        request.expiresAt = shortlink::ShortLinkService::parseUtcTimestamp(expires->second.value);
        if (!request.expiresAt)
        {
            return std::nullopt;
        }
    }
    return request;
}

std::optional<shortlink::ShortLinkRepository::LifecycleUpdate> parseLifecycleUpdate(
    const std::string& body)
{
    const std::optional<SimpleJsonObject> object = parseSimpleJsonObject(body);
    if (!object || object->empty() || object->size() > 2)
    {
        return std::nullopt;
    }

    shortlink::ShortLinkRepository::LifecycleUpdate update;
    for (const auto& item : *object)
    {
        if (item.first == "status")
        {
            if (item.second.isNull)
            {
                return std::nullopt;
            }
            update.status = shortlink::parseStatus(item.second.value);
            if (!update.status)
            {
                return std::nullopt;
            }
        }
        else if (item.first == "expires_at")
        {
            update.expiresAtProvided = true;
            if (!item.second.isNull)
            {
                update.expiresAt = shortlink::ShortLinkService::parseUtcTimestamp(item.second.value);
                if (!update.expiresAt)
                {
                    return std::nullopt;
                }
            }
        }
        else
        {
            return std::nullopt;
        }
    }
    return update;
}

std::string recordJson(const shortlink::ShortLinkRepository::ShortLinkRecord& record)
{
    const std::string expiresAt = record.expiresAt
                                      ? "\"" + shortlink::ShortLinkService::formatUtcTimestamp(
                                                    *record.expiresAt) +
                                            "\""
                                      : "null";
    return "{\"id\":" + std::to_string(record.id) +
           ",\"code\":\"" + http::utils::escapeJsonString(record.code) +
           "\",\"original_url\":\"" + http::utils::escapeJsonString(record.originalUrl) +
           "\",\"status\":\"" + shortlink::statusToString(record.status) +
           "\",\"expires_at\":" + expiresAt +
           ",\"created_at\":\"" +
           shortlink::ShortLinkService::formatUtcTimestamp(record.createdAt) +
           "\",\"updated_at\":\"" +
           shortlink::ShortLinkService::formatUtcTimestamp(record.updatedAt) + "\"}";
}

std::optional<std::uint64_t> parseUnsigned(const std::string& value)
{
    if (value.empty())
    {
        return std::nullopt;
    }
    std::uint64_t result = 0;
    for (const char ch : value)
    {
        if (ch < '0' || ch > '9')
        {
            return std::nullopt;
        }
        const unsigned digit = static_cast<unsigned>(ch - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return result;
}

void handleHealth(const http::HttpRequest& req, http::HttpResponse* resp)
{
    (void)req;
    resp->setStatusCode(http::HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setJsonBody("{\"status\":\"ok\"}");
}

void handleReadiness(const http::HttpRequest& req,
                     http::HttpResponse* resp,
                     bool requiresMySql)
{
    (void)req;
    const bool ready = !requiresMySql ||
                       http::db::DbConnectionPool::getInstance().isHealthy(
                           std::chrono::milliseconds(100));
    if (ready)
    {
        resp->setStatusCode(http::HttpResponse::k200Ok);
        resp->setStatusMessage("OK");
        resp->setJsonBody("{\"status\":\"ready\"}");
        return;
    }

    resp->setStatusCode(http::HttpResponse::k503ServiceUnavailable);
    resp->setStatusMessage("Service Unavailable");
    resp->setJsonBody("{\"status\":\"not_ready\"}");
}

void handleCreateShortLink(const http::HttpRequest& req,
                           http::HttpResponse* resp,
                           shortlink::ShortLinkService* service,
                           shortlink::ShortLinkMetrics* metrics,
                           shortlink::ShortLinkMetrics::Storage storage,
                           const shortlink::RateLimiter* rateLimiter)
{
    if (rateLimiter != nullptr)
    {
        const shortlink::RateLimiter::Result result = rateLimiter->check("global");
        if (result.status == shortlink::RateLimiter::Status::Allowed)
        {
            metrics->recordRateLimit(shortlink::ShortLinkMetrics::RateLimitResult::Allowed);
        }
        else if (result.status == shortlink::RateLimiter::Status::Limited)
        {
            metrics->recordRateLimit(shortlink::ShortLinkMetrics::RateLimitResult::Limited);
            LOG_WARN << "rate_limit result=limited request_id=" << req.requestId()
                     << " retry_after_seconds=" << result.retryAfterSeconds;
            resp->addHeader("Retry-After", std::to_string(result.retryAfterSeconds));
            resp->setErrorResponse(http::HttpResponse::k429TooManyRequests,
                                   "rate_limit_exceeded",
                                   "Too many requests");
            resp->setStatusMessage("Too Many Requests");
            return;
        }
        else
        {
            metrics->recordRateLimit(shortlink::ShortLinkMetrics::RateLimitResult::Error);
            metrics->recordBackendError(shortlink::ShortLinkMetrics::Backend::Redis,
                                        shortlink::ShortLinkMetrics::BackendOperation::RateLimit);
            LOG_ERROR << "rate_limit result=error fail_open=true request_id=" << req.requestId();
        }
    }

    if (!hasJsonContentType(req))
    {
        metrics->recordCreate(shortlink::ShortLinkMetrics::CreateResult::Invalid, storage);
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_request",
                               "Content-Type must be application/json");
        return;
    }

    const std::optional<CreateRequest> request = parseCreateRequest(req.getBody());
    if (!request)
    {
        metrics->recordCreate(shortlink::ShortLinkMetrics::CreateResult::Invalid, storage);
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_request",
                               "Request body must contain url and an optional UTC expires_at");
        return;
    }

    if (request->expiresAt && *request->expiresAt <= shortlink::ShortLinkService::nowEpochSeconds())
    {
        metrics->recordCreate(shortlink::ShortLinkMetrics::CreateResult::Invalid, storage);
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_expires_at",
                               "expires_at must be in the future");
        return;
    }

    std::optional<shortlink::ShortLinkService::ShortLink> shortLink;
    try
    {
        shortLink = service->createShortLink(request->url, request->expiresAt);
    }
    catch (...)
    {
        metrics->recordCreate(shortlink::ShortLinkMetrics::CreateResult::Error, storage);
        throw;
    }
    if (!shortLink)
    {
        metrics->recordCreate(shortlink::ShortLinkMetrics::CreateResult::Invalid, storage);
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_url",
                               "URL must start with http:// or https://");
        return;
    }

    const std::string body =
        "{\"code\":\"" + http::utils::escapeJsonString(shortLink->record.code) +
        "\",\"short_url\":\"" + http::utils::escapeJsonString(shortLink->shortUrl) +
        "\",\"original_url\":\"" + http::utils::escapeJsonString(shortLink->record.originalUrl) +
        "\",\"status\":\"active\",\"expires_at\":" +
        (shortLink->record.expiresAt
             ? "\"" + shortlink::ShortLinkService::formatUtcTimestamp(*shortLink->record.expiresAt) + "\""
             : "null") +
        "}";

    setJsonResponse(resp,
                    http::HttpResponse::k201Created,
                    "Created",
                    body);
    metrics->recordCreate(shortlink::ShortLinkMetrics::CreateResult::Success, storage);
}

void handleRedirect(const http::HttpRequest& req,
                    http::HttpResponse* resp,
                    shortlink::ShortLinkService* service)
{
    const std::string code = req.getPathParameters("param1");
    const shortlink::ShortLinkService::RedirectResult result = service->resolve(code);
    if (result.status != shortlink::ShortLinkService::RedirectStatus::Success)
    {
        resp->setErrorResponse(http::HttpResponse::k404NotFound,
                               "short_link_not_found",
                               "Short link not found");
        return;
    }

    resp->setRedirect(*result.originalUrl);
}

void handleInternalDetail(const http::HttpRequest& req,
                          http::HttpResponse* resp,
                          shortlink::ShortLinkService* service)
{
    const std::optional<shortlink::ShortLinkRepository::ShortLinkRecord> record =
        service->get(req.getPathParameters("param1"));
    if (!record)
    {
        resp->setErrorResponse(http::HttpResponse::k404NotFound,
                               "short_link_not_found",
                               "Short link not found");
        return;
    }
    setJsonResponse(resp, http::HttpResponse::k200Ok, "OK", recordJson(*record));
}

void handleInternalList(const http::HttpRequest& req,
                        http::HttpResponse* resp,
                        shortlink::ShortLinkService* service)
{
    shortlink::ShortLinkRepository::ListQuery query;
    std::size_t requestedLimit = 50;
    const std::string cursorValue = req.getQueryParameters("cursor");
    if (!cursorValue.empty())
    {
        const std::optional<std::uint64_t> cursor = parseUnsigned(cursorValue);
        if (!cursor)
        {
            resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_cursor",
                                   "cursor must be an unsigned integer");
            return;
        }
        query.cursor = *cursor;
    }
    const std::string limitValue = req.getQueryParameters("limit");
    if (!limitValue.empty())
    {
        const std::optional<std::uint64_t> limit = parseUnsigned(limitValue);
        if (!limit || *limit < 1 || *limit > 100)
        {
            resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_limit",
                                   "limit must be between 1 and 100");
            return;
        }
        requestedLimit = static_cast<std::size_t>(*limit);
    }
    const std::string statusValue = req.getQueryParameters("status");
    if (!statusValue.empty())
    {
        query.status = shortlink::parseStatus(statusValue);
        if (!query.status)
        {
            resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_status",
                                   "status must be active or disabled");
            return;
        }
    }

    query.limit = requestedLimit + 1;
    std::vector<shortlink::ShortLinkRepository::ShortLinkRecord> records = service->list(query);
    const bool hasMore = records.size() > requestedLimit;
    if (hasMore)
    {
        records.resize(requestedLimit);
    }

    std::string body = "{\"items\":[";
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        if (i > 0)
        {
            body += ',';
        }
        body += recordJson(records[i]);
    }
    body += "],\"next_cursor\":";
    body += hasMore && !records.empty() ? std::to_string(records.back().id) : "null";
    body += "}";
    setJsonResponse(resp, http::HttpResponse::k200Ok, "OK", body);
}

void handleInternalUpdate(const http::HttpRequest& req,
                          http::HttpResponse* resp,
                          shortlink::ShortLinkService* service)
{
    if (!hasJsonContentType(req))
    {
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_request",
                               "Content-Type must be application/json");
        return;
    }
    const std::optional<shortlink::ShortLinkRepository::LifecycleUpdate> update =
        parseLifecycleUpdate(req.getBody());
    if (!update)
    {
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_request",
                               "Only status and expires_at may be updated");
        return;
    }

    const std::optional<shortlink::ShortLinkRepository::ShortLinkRecord> record =
        service->updateLifecycle(req.getPathParameters("param1"), *update);
    if (!record)
    {
        resp->setErrorResponse(http::HttpResponse::k404NotFound,
                               "short_link_not_found",
                               "Short link not found");
        return;
    }
    setJsonResponse(resp, http::HttpResponse::k200Ok, "OK", recordJson(*record));
}

void handleMetrics(const http::HttpRequest& req,
                   http::HttpResponse* resp,
                   const http::HttpServer* server,
                   const shortlink::ShortLinkMetrics* shortLinkMetrics)
{
    (void)req;
    const std::string body = server->prometheusMetrics() + shortLinkMetrics->renderPrometheus();
    resp->setStatusCode(http::HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setContentType("text/plain; version=0.0.4; charset=utf-8");
    resp->setContentLength(body.size());
    resp->setBody(body);
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string configPath = argc > 1 ? argv[1] : "apps/shortlink_server/config/server.conf.example";
    const ServerConfig config = loadServerConfig(configPath);

    LOG_INFO << "Starting " << config.name << " on port " << config.port
             << " with " << config.threadNum << " worker threads";

    http::HttpServer server(config.port, config.name);
    shortlink::ShortLinkMetrics shortLinkMetrics;
    const shortlink::ShortLinkMetrics::Storage metricsStorage =
        config.storageType == "mysql"
            ? shortlink::ShortLinkMetrics::Storage::Mysql
            : shortlink::ShortLinkMetrics::Storage::Memory;
    std::unique_ptr<shortlink::ShortLinkRepository> primaryShortLinkRepository;
    std::unique_ptr<shortlink::ShortLinkRepository> shortLinkRepository;
    std::unique_ptr<shortlink::RateLimiter> rateLimiter;
    if (config.storageType == "mysql")
    {
        http::db::DbConnectionPool::getInstance().init(config.mysqlHost,
                                                       config.mysqlUser,
                                                       config.mysqlPassword,
                                                       config.mysqlDatabase,
                                                       static_cast<std::size_t>(config.mysqlPoolSize));
        primaryShortLinkRepository =
            std::make_unique<shortlink::MySqlShortLinkRepository>(&shortLinkMetrics);
        if (config.redisEnabled)
        {
            shortlink::RedisShortLinkCache::Config redisConfig;
            redisConfig.host = config.redisHost;
            redisConfig.port = config.redisPort;
            redisConfig.database = config.redisDatabase;
            redisConfig.ttlSeconds = config.redisTtlSeconds;
            redisConfig.keyPrefix = config.redisKeyPrefix;
            shortLinkRepository = std::make_unique<shortlink::RedisCachedShortLinkRepository>(
                *primaryShortLinkRepository,
                shortlink::RedisShortLinkCache(redisConfig, &shortLinkMetrics),
                &shortLinkMetrics);
        }
        else
        {
            shortLinkRepository = std::move(primaryShortLinkRepository);
        }
    }
    else
    {
        if (config.redisEnabled)
        {
            LOG_INFO << "redis.enabled is ignored when storage.type is not mysql";
        }
        shortLinkRepository =
            std::make_unique<shortlink::MemoryShortLinkRepository>(&shortLinkMetrics);
    }

    if (config.rateLimitEnabled)
    {
        shortlink::RedisFixedWindowRateLimiter::Config rateLimitConfig;
        rateLimitConfig.host = config.redisHost;
        rateLimitConfig.port = config.redisPort;
        rateLimitConfig.database = config.redisDatabase;
        rateLimitConfig.requests = config.rateLimitRequests;
        rateLimitConfig.windowSeconds = config.rateLimitWindowSeconds;
        rateLimitConfig.keyPrefix = config.rateLimitKeyPrefix;
        rateLimiter = std::make_unique<shortlink::RedisFixedWindowRateLimiter>(
            std::move(rateLimitConfig));
    }

    shortlink::ShortLinkService shortLinkService(*shortLinkRepository, &shortLinkMetrics);

    server.setThreadNum(config.threadNum);
    server.Get("/api/health", handleHealth);
    server.Get("/api/health/live", handleHealth);
    server.Get("/api/health/ready", [requiresMySql = config.storageType == "mysql"](
                                        const http::HttpRequest& req,
                                        http::HttpResponse* resp) {
        handleReadiness(req, resp, requiresMySql);
    });
    server.Post("/api/short-links", [&shortLinkService,
                                     &shortLinkMetrics,
                                     metricsStorage,
                                     limiter = rateLimiter.get()](const http::HttpRequest& req,
                                                                  http::HttpResponse* resp) {
        handleCreateShortLink(req,
                              resp,
                              &shortLinkService,
                              &shortLinkMetrics,
                              metricsStorage,
                              limiter);
    });
    server.addRoute(http::HttpRequest::kGet,
                    "/s/:code",
                    [&shortLinkService](const http::HttpRequest& req, http::HttpResponse* resp) {
                        handleRedirect(req, resp, &shortLinkService);
                    });
    server.Get("/internal/short-links", [&shortLinkService](const http::HttpRequest& req,
                                                            http::HttpResponse* resp) {
        handleInternalList(req, resp, &shortLinkService);
    });
    server.addRoute(http::HttpRequest::kGet,
                    "/internal/short-links/:code",
                    [&shortLinkService](const http::HttpRequest& req, http::HttpResponse* resp) {
                        handleInternalDetail(req, resp, &shortLinkService);
                    });
    server.addRoute(http::HttpRequest::kPut,
                    "/internal/short-links/:code",
                    [&shortLinkService](const http::HttpRequest& req, http::HttpResponse* resp) {
                        handleInternalUpdate(req, resp, &shortLinkService);
                    });
    if (config.metricsEnabled)
    {
        server.Get("/metrics", [&server, &shortLinkMetrics](const http::HttpRequest& req,
                                                            http::HttpResponse* resp) {
            handleMetrics(req, resp, &server, &shortLinkMetrics);
        });
    }
    server.start();
}
