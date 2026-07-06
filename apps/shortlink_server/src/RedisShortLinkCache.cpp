#include "shortlink/RedisShortLinkCache.h"

#include <memory>
#include <utility>

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

RedisReplyPtr makeReply(redisContext* context, const char* command)
{
    return RedisReplyPtr(static_cast<redisReply*>(redisCommand(context, command)));
}

} // namespace

RedisShortLinkCache::RedisShortLinkCache(Config config)
    : config_(std::move(config))
{}

std::optional<std::string> RedisShortLinkCache::getOriginalUrl(const std::string& code) const
{
    struct timeval timeout
    {
        1, 0
    };
    RedisContextPtr context(redisConnectWithTimeout(config_.host.c_str(), config_.port, timeout));
    if (!context || context->err)
    {
        LOG_ERROR << "Redis cache get connection failed";
        return std::nullopt;
    }

    if (config_.database > 0)
    {
        RedisReplyPtr selectReply(makeReply(context.get(), ("SELECT " + std::to_string(config_.database)).c_str()));
        if (!selectReply || selectReply->type == REDIS_REPLY_ERROR)
        {
            LOG_ERROR << "Redis cache get SELECT failed";
            return std::nullopt;
        }
    }

    const std::string key = buildCacheKey(config_, code);
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context.get(), "GET %b", key.data(), key.size())));
    if (!reply)
    {
        LOG_ERROR << "Redis cache get command failed";
        return std::nullopt;
    }

    if (reply->type == REDIS_REPLY_NIL)
    {
        return std::nullopt;
    }

    if (reply->type != REDIS_REPLY_STRING)
    {
        LOG_ERROR << "Redis cache get returned unexpected reply type";
        return std::nullopt;
    }

    return std::string(reply->str, static_cast<std::size_t>(reply->len));
}

bool RedisShortLinkCache::setOriginalUrl(const std::string& code, const std::string& originalUrl) const
{
    struct timeval timeout
    {
        1, 0
    };
    RedisContextPtr context(redisConnectWithTimeout(config_.host.c_str(), config_.port, timeout));
    if (!context || context->err)
    {
        LOG_ERROR << "Redis cache set connection failed";
        return false;
    }

    if (config_.database > 0)
    {
        RedisReplyPtr selectReply(makeReply(context.get(), ("SELECT " + std::to_string(config_.database)).c_str()));
        if (!selectReply || selectReply->type == REDIS_REPLY_ERROR)
        {
            LOG_ERROR << "Redis cache set SELECT failed";
            return false;
        }
    }

    const std::string key = buildCacheKey(config_, code);
    RedisReplyPtr reply;
    if (config_.ttlSeconds > 0)
    {
        reply.reset(static_cast<redisReply*>(redisCommand(context.get(),
                                                          "SETEX %b %d %b",
                                                          key.data(),
                                                          key.size(),
                                                          config_.ttlSeconds,
                                                          originalUrl.data(),
                                                          originalUrl.size())));
    }
    else
    {
        reply.reset(static_cast<redisReply*>(redisCommand(context.get(),
                                                          "SET %b %b",
                                                          key.data(),
                                                          key.size(),
                                                          originalUrl.data(),
                                                          originalUrl.size())));
    }

    if (!reply || reply->type == REDIS_REPLY_ERROR)
    {
        LOG_ERROR << "Redis cache set command failed";
        return false;
    }

    return reply->type == REDIS_REPLY_STATUS;
}

} // namespace shortlink
