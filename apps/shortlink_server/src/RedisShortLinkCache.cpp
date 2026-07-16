#include "shortlink/RedisShortLinkCache.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

#include <hiredis/hiredis.h>
#include <muduo/base/Logging.h>

namespace shortlink
{
namespace
{

struct RedisContextDeleter
{
    void operator()(redisContext* context) const
    {
        if (context)
        {
            redisFree(context);
        }
    }
};

struct RedisReplyDeleter
{
    void operator()(redisReply* reply) const
    {
        if (reply)
        {
            freeReplyObject(reply);
        }
    }
};

using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

std::string buildCacheKey(const RedisShortLinkCache::Config& config, const std::string& code)
{
    return config.keyPrefix + code;
}

RedisContextPtr connect(const RedisShortLinkCache::Config& config)
{
    struct timeval timeout
    {
        1, 0
    };
    RedisContextPtr context(redisConnectWithTimeout(config.host.c_str(), config.port, timeout));
    if (!context || context->err)
    {
        return nullptr;
    }
    if (config.database > 0)
    {
        RedisReplyPtr reply(static_cast<redisReply*>(
            redisCommand(context.get(), "SELECT %d", config.database)));
        if (!reply || reply->type == REDIS_REPLY_ERROR)
        {
            return nullptr;
        }
    }
    return context;
}

std::string serialize(const ShortLinkRepository::ShortLinkRecord& record)
{
    return "v1|" + std::to_string(record.id) + "|" + statusToString(record.status) + "|" +
           (record.expiresAt ? std::to_string(*record.expiresAt) : "-") + "|" +
           std::to_string(record.createdAt) + "|" + std::to_string(record.updatedAt) + "|" +
           record.originalUrl;
}

std::optional<ShortLinkRepository::ShortLinkRecord> deserialize(
    const std::string& code,
    const std::string& value)
{
    if (value.compare(0, 3, "v1|") != 0)
    {
        return std::nullopt;
    }

    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (int separator = 0; separator < 6; ++separator)
    {
        const std::size_t end = value.find('|', begin);
        if (end == std::string::npos)
        {
            return std::nullopt;
        }
        fields.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    fields.push_back(value.substr(begin));
    if (fields.size() != 7 || fields[0] != "v1" || fields[6].empty())
    {
        return std::nullopt;
    }

    try
    {
        const std::optional<ShortLinkRepository::Status> status = parseStatus(fields[2]);
        if (!status)
        {
            return std::nullopt;
        }
        ShortLinkRepository::ShortLinkRecord record;
        record.id = std::stoull(fields[1]);
        record.code = code;
        record.originalUrl = fields[6];
        record.status = *status;
        if (fields[3] != "-")
        {
            record.expiresAt = std::stoll(fields[3]);
        }
        record.createdAt = std::stoll(fields[4]);
        record.updatedAt = std::stoll(fields[5]);
        return record;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::int64_t nowEpochSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

RedisShortLinkCache::RedisShortLinkCache(Config config, ShortLinkMetrics* metrics)
    : config_(std::move(config)),
      metrics_(metrics)
{
    if (config_.ttlSeconds <= 0 || config_.ttlSeconds > 86400)
    {
        config_.ttlSeconds = 3600;
    }
}

std::optional<ShortLinkRepository::ShortLinkRecord> RedisShortLinkCache::get(
    const std::string& code) const
{
    RedisContextPtr context = connect(config_);
    if (!context)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordCache(ShortLinkMetrics::CacheOperation::Get,
                                  ShortLinkMetrics::CacheResult::Error);
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                         ShortLinkMetrics::BackendOperation::Get);
        }
        LOG_ERROR << "Redis cache get connection failed";
        return std::nullopt;
    }

    const std::string key = buildCacheKey(config_, code);
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context.get(), "GET %b", key.data(), key.size())));
    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordCache(ShortLinkMetrics::CacheOperation::Get,
                                  ShortLinkMetrics::CacheResult::Error);
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                         ShortLinkMetrics::BackendOperation::Get);
        }
        LOG_ERROR << "Redis cache get command failed";
        return std::nullopt;
    }
    if (reply->type == REDIS_REPLY_NIL)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordCache(ShortLinkMetrics::CacheOperation::Get,
                                  ShortLinkMetrics::CacheResult::Miss);
        }
        return std::nullopt;
    }
    if (reply->type != REDIS_REPLY_STRING)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordCache(ShortLinkMetrics::CacheOperation::Get,
                                  ShortLinkMetrics::CacheResult::Error);
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                         ShortLinkMetrics::BackendOperation::Get);
        }
        LOG_ERROR << "Redis cache get returned unexpected reply type";
        return std::nullopt;
    }

    const std::string value(reply->str, static_cast<std::size_t>(reply->len));
    const std::optional<ShortLinkRepository::ShortLinkRecord> record = deserialize(code, value);
    if (!record)
    {
        RedisReplyPtr deleteReply(static_cast<redisReply*>(
            redisCommand(context.get(), "DEL %b", key.data(), key.size())));
        (void)deleteReply;
        if (metrics_ != nullptr)
        {
            metrics_->recordCache(ShortLinkMetrics::CacheOperation::Get,
                                  ShortLinkMetrics::CacheResult::Miss);
        }
        LOG_INFO << "Discarded legacy or invalid Redis short link cache entry code=" << code;
        return std::nullopt;
    }

    if (metrics_ != nullptr)
    {
        metrics_->recordCache(ShortLinkMetrics::CacheOperation::Get,
                              ShortLinkMetrics::CacheResult::Hit);
    }
    return record;
}

bool RedisShortLinkCache::set(const ShortLinkRepository::ShortLinkRecord& record) const
{
    int ttl = config_.ttlSeconds;
    if (record.expiresAt)
    {
        const std::int64_t remaining = *record.expiresAt - nowEpochSeconds();
        if (remaining <= 0)
        {
            return erase(record.code);
        }
        ttl = ttl > 0 ? std::min<std::int64_t>(ttl, remaining)
                      : static_cast<int>(std::min<std::int64_t>(remaining, 2147483647));
    }

    RedisContextPtr context = connect(config_);
    if (!context)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordCache(ShortLinkMetrics::CacheOperation::Set,
                                  ShortLinkMetrics::CacheResult::Error);
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                         ShortLinkMetrics::BackendOperation::Set);
        }
        LOG_ERROR << "Redis cache set connection failed";
        return false;
    }

    const std::string key = buildCacheKey(config_, record.code);
    const std::string value = serialize(record);
    RedisReplyPtr reply;
    if (ttl > 0)
    {
        reply.reset(static_cast<redisReply*>(redisCommand(context.get(),
                                                          "SETEX %b %d %b",
                                                          key.data(),
                                                          key.size(),
                                                          ttl,
                                                          value.data(),
                                                          value.size())));
    }
    else
    {
        reply.reset(static_cast<redisReply*>(redisCommand(context.get(),
                                                          "SET %b %b",
                                                          key.data(),
                                                          key.size(),
                                                          value.data(),
                                                          value.size())));
    }

    const bool success = reply && reply->type == REDIS_REPLY_STATUS;
    if (metrics_ != nullptr)
    {
        metrics_->recordCache(ShortLinkMetrics::CacheOperation::Set,
                              success ? ShortLinkMetrics::CacheResult::Success
                                      : ShortLinkMetrics::CacheResult::Error);
        if (!success)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                         ShortLinkMetrics::BackendOperation::Set);
        }
    }
    if (!success)
    {
        LOG_ERROR << "Redis cache set command failed";
    }
    return success;
}

bool RedisShortLinkCache::erase(const std::string& code) const
{
    RedisContextPtr context = connect(config_);
    if (!context)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordCache(ShortLinkMetrics::CacheOperation::Delete,
                                  ShortLinkMetrics::CacheResult::Error);
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                         ShortLinkMetrics::BackendOperation::Delete);
        }
        LOG_ERROR << "Redis cache delete connection failed code=" << code;
        return false;
    }

    const std::string key = buildCacheKey(config_, code);
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context.get(), "DEL %b", key.data(), key.size())));
    const bool success = reply && reply->type == REDIS_REPLY_INTEGER;
    if (metrics_ != nullptr)
    {
        metrics_->recordCache(ShortLinkMetrics::CacheOperation::Delete,
                              success ? ShortLinkMetrics::CacheResult::Success
                                      : ShortLinkMetrics::CacheResult::Error);
        if (!success)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Redis,
                                         ShortLinkMetrics::BackendOperation::Delete);
        }
    }
    if (!success)
    {
        LOG_ERROR << "Redis cache delete command failed code=" << code;
    }
    return success;
}

} // namespace shortlink
