#include "shortlink/ShortLinkHttpApi.h"

#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/HttpServer.h"
#include "shortlink/AccessEventPublisher.h"
#include "shortlink/AccessStatistics.h"
#include "shortlink/AccessStatisticsRepository.h"
#include "shortlink/AuthService.h"
#include "shortlink/RateLimiter.h"
#include "shortlink/RedirectHandler.h"
#include "shortlink/SameOriginPolicy.h"
#include "shortlink/ShortLinkRepository.h"
#include "shortlink/ShortLinkService.h"

#include "utils/db/DbConnectionPool.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <muduo/base/Logging.h>

namespace shortlink
{

namespace
{

using Json = nlohmann::json;

bool hasJsonContentType(const http::HttpRequest& request)
{
    return request.getHeader("Content-Type").find("application/json") != std::string::npos;
}

void setJson(http::HttpResponse* response,
             http::HttpResponse::HttpStatusCode status,
             const std::string& message,
             const Json& body)
{
    response->setStatusCode(status);
    response->setStatusMessage(message);
    response->setJsonBody(body.dump());
}

Json recordJson(const ShortLinkRepository::ShortLinkRecord& record)
{
    Json body {
        { "id", record.id },
        { "code", record.code },
        { "short_url", "/s/" + record.code },
        { "original_url", record.originalUrl },
        { "status", statusToString(record.status) },
        { "expires_at", nullptr },
        { "created_at", ShortLinkService::formatUtcTimestamp(record.createdAt) },
        { "updated_at", ShortLinkService::formatUtcTimestamp(record.updatedAt) }
    };
    if (record.expiresAt)
    {
        body["expires_at"] = ShortLinkService::formatUtcTimestamp(*record.expiresAt);
    }
    return body;
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

bool authenticate(const http::HttpRequest& request,
                  http::HttpResponse* response,
                  const AuthService& service,
                  const AuthHttpConfig& config,
                  AuthRepository::User* user)
{
    const bool authenticated = requireAuthenticatedUser(request, response, service, config, user);
    if (authenticated)
    {
        response->addHeader("Cache-Control", "no-store");
    }
    return authenticated;
}

void handleHealth(const http::HttpRequest&, http::HttpResponse* response)
{
    setJson(response, http::HttpResponse::k200Ok, "OK", Json { { "status", "ok" } });
}

void handleReadiness(const http::HttpRequest&,
                     http::HttpResponse* response,
                     bool requiresMySql)
{
    const bool ready = !requiresMySql ||
                       http::db::DbConnectionPool::getInstance().isHealthy(
                           std::chrono::milliseconds(100));
    if (ready)
    {
        setJson(response,
                http::HttpResponse::k200Ok,
                "OK",
                Json { { "status", "ready" } });
        return;
    }
    setJson(response,
            http::HttpResponse::k503ServiceUnavailable,
            "Service Unavailable",
            Json { { "status", "not_ready" } });
}

std::optional<Json> parseObject(const http::HttpRequest& request,
                                http::HttpResponse* response)
{
    if (!hasJsonContentType(request) || request.getBody().size() > 16384)
    {
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_request",
                                   "Content-Type must be application/json");
        return std::nullopt;
    }
    Json object = Json::parse(request.getBody(), nullptr, false, true);
    if (object.is_discarded() || !object.is_object())
    {
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_request",
                                   "Request body must be a JSON object");
        return std::nullopt;
    }
    return object;
}

void handleCreate(const http::HttpRequest& request,
                  http::HttpResponse* response,
                  ShortLinkService* service,
                  ShortLinkMetrics* metrics,
                  ShortLinkMetrics::Storage storage,
                  const RateLimiter* rateLimiter,
                  std::uint64_t ownerId)
{
    if (rateLimiter)
    {
        const RateLimiter::Result result = rateLimiter->check("global");
        if (result.status == RateLimiter::Status::Allowed)
        {
            metrics->recordRateLimit(ShortLinkMetrics::RateLimitResult::Allowed);
        }
        else if (result.status == RateLimiter::Status::Limited)
        {
            metrics->recordRateLimit(ShortLinkMetrics::RateLimitResult::Limited);
            response->addHeader("Retry-After", std::to_string(result.retryAfterSeconds));
            response->setErrorResponse(http::HttpResponse::k429TooManyRequests,
                                       "rate_limit_exceeded",
                                       "Too many requests");
            response->setStatusMessage("Too Many Requests");
            return;
        }
        else
        {
            metrics->recordRateLimit(ShortLinkMetrics::RateLimitResult::Error);
            metrics->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                        ShortLinkMetrics::BackendOperation::RateLimit);
            LOG_ERROR << "rate_limit result=error fail_open=true request_id="
                      << request.requestId();
        }
    }

    const auto object = parseObject(request, response);
    if (!object)
    {
        metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
        return;
    }
    for (const auto& item : object->items())
    {
        if (item.key() != "url" && item.key() != "expires_at" && item.key() != "custom_code")
        {
            metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_request",
                                       "Unknown create field");
            return;
        }
    }
    if (!object->contains("url") || !(*object)["url"].is_string())
    {
        metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_request",
                                   "url must be a string");
        return;
    }

    std::optional<std::int64_t> expiresAt;
    if (object->contains("expires_at") && !(*object)["expires_at"].is_null())
    {
        if (!(*object)["expires_at"].is_string() ||
            !(expiresAt = ShortLinkService::parseUtcTimestamp(
                  (*object)["expires_at"].get<std::string>())))
        {
            metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_expires_at",
                                       "expires_at must be a UTC timestamp");
            return;
        }
        if (*expiresAt <= ShortLinkService::nowEpochSeconds())
        {
            metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_expires_at",
                                       "expires_at must be in the future");
            return;
        }
    }
    std::optional<std::string> customCode;
    if (object->contains("custom_code") && !(*object)["custom_code"].is_null())
    {
        if (!(*object)["custom_code"].is_string())
        {
            metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_custom_code",
                                       "custom_code must be a string");
            return;
        }
        customCode = (*object)["custom_code"].get<std::string>();
    }

    try
    {
        const auto created = service->createShortLink((*object)["url"].get<std::string>(),
                                                      expiresAt,
                                                      ownerId,
                                                      customCode);
        if (!created)
        {
            metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
            response->setErrorResponse(
                http::HttpResponse::k400BadRequest,
                customCode ? "invalid_custom_code" : "invalid_url",
                customCode ? "custom_code must contain 4-32 allowed characters"
                           : "URL must start with http:// or https:// and expiry must be future");
            return;
        }
        setJson(response, http::HttpResponse::k201Created, "Created", recordJson(created->record));
        metrics->recordCreate(ShortLinkMetrics::CreateResult::Success, storage);
    }
    catch (const ShortLinkRepository::ShortCodeConflict&)
    {
        metrics->recordCreate(ShortLinkMetrics::CreateResult::Invalid, storage);
        response->setErrorResponse(http::HttpResponse::k409Conflict,
                                   "short_code_conflict",
                                   "Short code already exists");
    }
    catch (...)
    {
        metrics->recordCreate(ShortLinkMetrics::CreateResult::Error, storage);
        throw;
    }
}

void handleDetail(const http::HttpRequest& request,
                  http::HttpResponse* response,
                  const ShortLinkService& service,
                  std::uint64_t ownerId)
{
    const auto record = service.getForOwner(request.getPathParameters("param1"), ownerId);
    if (!record)
    {
        response->setErrorResponse(http::HttpResponse::k404NotFound,
                                   "short_link_not_found",
                                   "Short link not found");
        return;
    }
    setJson(response, http::HttpResponse::k200Ok, "OK", recordJson(*record));
}

void handleList(const http::HttpRequest& request,
                http::HttpResponse* response,
                const ShortLinkService& service,
                std::uint64_t ownerId)
{
    ShortLinkRepository::ListQuery query;
    query.ownerId = ownerId;
    std::size_t requestedLimit = 50;
    const std::string cursorValue = request.getQueryParameters("cursor");
    if (!cursorValue.empty())
    {
        const auto cursor = parseUnsigned(cursorValue);
        if (!cursor)
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_cursor",
                                       "cursor must be an unsigned integer");
            return;
        }
        query.cursor = *cursor;
    }
    const std::string limitValue = request.getQueryParameters("limit");
    if (!limitValue.empty())
    {
        const auto limit = parseUnsigned(limitValue);
        if (!limit || *limit < 1 || *limit > 100)
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_limit",
                                       "limit must be between 1 and 100");
            return;
        }
        requestedLimit = static_cast<std::size_t>(*limit);
    }
    const std::string statusValue = request.getQueryParameters("status");
    if (!statusValue.empty())
    {
        query.status = parseStatus(statusValue);
        if (!query.status)
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_status",
                                       "status must be active or disabled");
            return;
        }
    }

    query.limit = requestedLimit + 1;
    auto records = service.list(query);
    const bool hasMore = records.size() > requestedLimit;
    if (hasMore)
    {
        records.resize(requestedLimit);
    }
    Json items = Json::array();
    for (const auto& record : records)
    {
        items.push_back(recordJson(record));
    }
    Json body { { "items", items }, { "next_cursor", nullptr } };
    if (hasMore && !records.empty())
    {
        body["next_cursor"] = records.back().id;
    }
    setJson(response, http::HttpResponse::k200Ok, "OK", body);
}

void handleUpdate(const http::HttpRequest& request,
                  http::HttpResponse* response,
                  ShortLinkService* service,
                  std::uint64_t ownerId)
{
    const auto object = parseObject(request, response);
    if (!object || object->empty() || object->size() > 2)
    {
        if (object)
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_request",
                                       "Only status and expires_at may be updated");
        }
        return;
    }
    ShortLinkRepository::LifecycleUpdate update;
    update.ownerId = ownerId;
    for (const auto& item : object->items())
    {
        if (item.key() == "status" && item.value().is_string())
        {
            update.status = parseStatus(item.value().get<std::string>());
            if (!update.status)
            {
                response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                           "invalid_status",
                                           "status must be active or disabled");
                return;
            }
        }
        else if (item.key() == "expires_at")
        {
            update.expiresAtProvided = true;
            if (!item.value().is_null())
            {
                if (!item.value().is_string() ||
                    !(update.expiresAt = ShortLinkService::parseUtcTimestamp(
                          item.value().get<std::string>())))
                {
                    response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                               "invalid_expires_at",
                                               "expires_at must be null or a UTC timestamp");
                    return;
                }
            }
        }
        else
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_request",
                                       "Only status and expires_at may be updated");
            return;
        }
    }
    const auto record = service->updateLifecycle(request.getPathParameters("param1"), update);
    if (!record)
    {
        response->setErrorResponse(http::HttpResponse::k404NotFound,
                                   "short_link_not_found",
                                   "Short link not found");
        return;
    }
    setJson(response, http::HttpResponse::k200Ok, "OK", recordJson(*record));
}

std::optional<AccessStatisticsQuery> parseStatisticsQuery(const http::HttpRequest& request,
                                                          http::HttpResponse* response)
{
    AccessStatisticsQuery query;
    const std::string intervalValue = request.getQueryParameters("interval");
    if (!intervalValue.empty())
    {
        const auto interval = parseAccessStatisticsInterval(intervalValue);
        if (!interval)
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_interval",
                                       "interval must be hour or day");
            return std::nullopt;
        }
        query.interval = *interval;
    }
    const std::int64_t bucketSeconds = accessStatisticsBucketSeconds(query.interval);
    const std::int64_t now = ShortLinkService::nowEpochSeconds();
    query.toEpochSeconds = ((now + bucketSeconds - 1) / bucketSeconds) * bucketSeconds;
    query.fromEpochSeconds = query.toEpochSeconds - 7 * 86400;

    const std::string fromValue = request.getQueryParameters("from");
    const std::string toValue = request.getQueryParameters("to");
    if (!fromValue.empty())
    {
        const auto from = ShortLinkService::parseUtcTimestamp(fromValue);
        if (!from)
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_from",
                                       "from must be a UTC timestamp");
            return std::nullopt;
        }
        query.fromEpochSeconds = *from;
    }
    if (!toValue.empty())
    {
        const auto to = ShortLinkService::parseUtcTimestamp(toValue);
        if (!to)
        {
            response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                       "invalid_to",
                                       "to must be a UTC timestamp");
            return std::nullopt;
        }
        query.toEpochSeconds = *to;
    }
    if (query.fromEpochSeconds % bucketSeconds != 0 ||
        query.toEpochSeconds % bucketSeconds != 0 ||
        query.toEpochSeconds <= query.fromEpochSeconds)
    {
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_statistics_range",
                                   "from/to must be aligned and increasing");
        return std::nullopt;
    }
    const std::int64_t maxRange = query.interval == AccessStatisticsInterval::Hour
                                      ? 31 * 86400
                                      : 366 * 86400;
    if (query.toEpochSeconds - query.fromEpochSeconds > maxRange)
    {
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "statistics_range_too_large",
                                   "Requested statistics range is too large");
        return std::nullopt;
    }
    return query;
}

std::string formatUtcTimestampMs(std::int64_t epochMilliseconds)
{
    std::string timestamp = ShortLinkService::formatUtcTimestamp(epochMilliseconds / 1000);
    timestamp.pop_back();
    std::ostringstream output;
    output << timestamp << '.' << std::setw(3) << std::setfill('0')
           << epochMilliseconds % 1000 << 'Z';
    return output.str();
}

Json countsJson(const AccessStatisticsResultCounts& counts)
{
    return Json { { "success", counts[0] },
                  { "disabled", counts[1] },
                  { "expired", counts[2] },
                  { "error", counts[3] } };
}

void handleStatistics(const http::HttpRequest& request,
                      http::HttpResponse* response,
                      const ShortLinkService& service,
                      const AccessStatisticsRepository& repository,
                      std::uint64_t ownerId)
{
    const std::string code = request.getPathParameters("param1");
    if (!service.getForOwner(code, ownerId))
    {
        response->setErrorResponse(http::HttpResponse::k404NotFound,
                                   "short_link_not_found",
                                   "Short link not found");
        return;
    }
    const auto query = parseStatisticsQuery(request, response);
    if (!query)
    {
        return;
    }
    const auto snapshot = repository.get(code, *query);
    if (!snapshot)
    {
        response->setErrorResponse(http::HttpResponse::k404NotFound,
                                   "short_link_not_found",
                                   "Short link not found");
        return;
    }
    const auto& summary = snapshot->summary;
    Json body {
        { "code", snapshot->code },
        { "consistency", "eventual" },
        { "summary",
          { { "access_count", summary.resultCounts[0] },
            { "attempt_count", accessStatisticsAttemptCount(summary.resultCounts) },
            { "result_counts", countsJson(summary.resultCounts) },
            { "last_access_at", nullptr },
            { "last_attempt_at", nullptr } } },
        { "trend",
          { { "interval", accessStatisticsIntervalToString(query->interval) },
            { "from", ShortLinkService::formatUtcTimestamp(query->fromEpochSeconds) },
            { "to", ShortLinkService::formatUtcTimestamp(query->toEpochSeconds) },
            { "points", Json::array() } } }
    };
    if (summary.lastAccessAtMs)
    {
        body["summary"]["last_access_at"] = formatUtcTimestampMs(*summary.lastAccessAtMs);
    }
    if (summary.lastAttemptAtMs)
    {
        body["summary"]["last_attempt_at"] = formatUtcTimestampMs(*summary.lastAttemptAtMs);
    }
    for (const auto& point : snapshot->trend)
    {
        body["trend"]["points"].push_back(
            { { "bucket_start", ShortLinkService::formatUtcTimestamp(point.bucketStartEpochSeconds) },
              { "access_count", point.resultCounts[0] },
              { "attempt_count", accessStatisticsAttemptCount(point.resultCounts) },
              { "result_counts", countsJson(point.resultCounts) } });
    }
    setJson(response, http::HttpResponse::k200Ok, "OK", body);
}

void handleMetrics(const http::HttpRequest&,
                   http::HttpResponse* response,
                   const http::HttpServer& server,
                   const ShortLinkMetrics& metrics)
{
    const std::string body = server.prometheusMetrics() + metrics.renderPrometheus();
    response->setStatusCode(http::HttpResponse::k200Ok);
    response->setStatusMessage("OK");
    response->setContentType("text/plain; version=0.0.4; charset=utf-8");
    response->setContentLength(body.size());
    response->setBody(body);
}

} // namespace

void registerShortLinkHttpApi(http::HttpServer* server,
                              ShortLinkService* shortLinkService,
                              AuthService* authService,
                              ShortLinkMetrics* metrics,
                              const RateLimiter* rateLimiter,
                              AccessEventPublisher* accessEventPublisher,
                              const AccessStatisticsRepository* statisticsRepository,
                              ShortLinkHttpApiConfig config)
{
    server->Get("/api/health", handleHealth);
    server->Get("/api/health/live", handleHealth);
    server->Get("/api/health/ready", [requiresMySql = config.requiresMySql](
                                         const http::HttpRequest& request,
                                         http::HttpResponse* response) {
        handleReadiness(request, response, requiresMySql);
    });
    registerAuthRoutes(server, authService, config.auth);
    server->Post("/api/short-links",
                 [=](const http::HttpRequest& request, http::HttpResponse* response) {
                     if (!requireSameOriginRequest(request, response))
                     {
                         return;
                     }
                     AuthRepository::User user;
                     if (!authenticate(request, response, *authService, config.auth, &user))
                     {
                         return;
                     }
                     handleCreate(request,
                                  response,
                                  shortLinkService,
                                  metrics,
                                  config.metricsStorage,
                                  rateLimiter,
                                  user.id);
                 });
    server->Get("/api/short-links",
                [=](const http::HttpRequest& request, http::HttpResponse* response) {
                    AuthRepository::User user;
                    if (authenticate(request, response, *authService, config.auth, &user))
                    {
                        handleList(request, response, *shortLinkService, user.id);
                    }
                });
    server->addRoute(http::HttpRequest::kGet,
                     "/api/short-links/:code",
                     [=](const http::HttpRequest& request, http::HttpResponse* response) {
                         AuthRepository::User user;
                         if (authenticate(request, response, *authService, config.auth, &user))
                         {
                             handleDetail(request, response, *shortLinkService, user.id);
                         }
                     });
    server->addRoute(http::HttpRequest::kPut,
                     "/api/short-links/:code",
                     [=](const http::HttpRequest& request, http::HttpResponse* response) {
                         if (!requireSameOriginRequest(request, response))
                         {
                             return;
                         }
                         AuthRepository::User user;
                         if (authenticate(request, response, *authService, config.auth, &user))
                         {
                             handleUpdate(request, response, shortLinkService, user.id);
                         }
                     });
    if (statisticsRepository)
    {
        server->addRoute(http::HttpRequest::kGet,
                         "/api/short-links/:code/statistics",
                         [=](const http::HttpRequest& request, http::HttpResponse* response) {
                             AuthRepository::User user;
                             if (authenticate(request, response, *authService, config.auth, &user))
                             {
                                 handleStatistics(request,
                                                  response,
                                                  *shortLinkService,
                                                  *statisticsRepository,
                                                  user.id);
                             }
                         });
    }
    server->addRoute(http::HttpRequest::kGet,
                     "/s/:code",
                     [=](const http::HttpRequest& request, http::HttpResponse* response) {
                         handleRedirect(request,
                                        response,
                                        shortLinkService,
                                        accessEventPublisher);
                     });
    if (config.metricsEnabled)
    {
        server->Get("/metrics",
                    [=](const http::HttpRequest& request, http::HttpResponse* response) {
                        handleMetrics(request, response, *server, *metrics);
                    });
    }
}

} // namespace shortlink
