#include "shortlink/RedisFixedWindowRateLimiter.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <hiredis/hiredis.h>
#include <muduo/base/Logging.h>

namespace shortlink
{
namespace
{

const char kFixedWindowScript[] =
    "local current = redis.call('INCR', KEYS[1]) "
    "if current == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
    "local ttl = redis.call('TTL', KEYS[1]) "
    "if ttl < 0 then redis.call('EXPIRE', KEYS[1], ARGV[1]); ttl = tonumber(ARGV[1]) end "
    "local allowed = 0 "
    "if current <= tonumber(ARGV[2]) then allowed = 1 end "
    "return {allowed, ttl}";

struct RedisContextDeleter
{
    void operator()(redisContext* context) const
    {
        if (context != nullptr)
        {
            redisFree(context);
        }
    }
};

struct RedisReplyDeleter
{
    void operator()(redisReply* reply) const
    {
        if (reply != nullptr)
        {
            freeReplyObject(reply);
        }
    }
};

using RedisContextPtr = std::unique_ptr<redisContext, RedisContextDeleter>;
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

bool isErrorReply(const redisReply* reply)
{
    return reply == nullptr || reply->type == REDIS_REPLY_ERROR;
}

} // namespace

RedisFixedWindowRateLimiter::RedisFixedWindowRateLimiter(Config config)
    : config_(std::move(config))
{}

RateLimiter::Result RedisFixedWindowRateLimiter::check(const std::string& key) const
{
    struct timeval timeout
    {
        1, 0
    };
    RedisContextPtr context(redisConnectWithTimeout(config_.host.c_str(), config_.port, timeout));
    if (!context || context->err)
    {
        LOG_ERROR << "rate_limit result=error fail_open=true stage=connect";
        return { Status::Error, 0 };
    }
    if (redisSetTimeout(context.get(), timeout) != REDIS_OK)
    {
        LOG_ERROR << "rate_limit result=error fail_open=true stage=set_timeout";
        return { Status::Error, 0 };
    }

    if (config_.database > 0)
    {
        RedisReplyPtr selectReply(static_cast<redisReply*>(
            redisCommand(context.get(), "SELECT %d", config_.database)));
        if (isErrorReply(selectReply.get()))
        {
            LOG_ERROR << "rate_limit result=error fail_open=true stage=select";
            return { Status::Error, 0 };
        }
    }

    const std::string redisKey = config_.keyPrefix + key;
    const std::string windowSeconds = std::to_string(config_.windowSeconds);
    const std::string requestLimit = std::to_string(config_.requests);
    RedisReplyPtr reply(static_cast<redisReply*>(
        redisCommand(context.get(),
                     "EVAL %b 1 %b %b %b",
                     kFixedWindowScript,
                     sizeof(kFixedWindowScript) - 1,
                     redisKey.data(),
                     redisKey.size(),
                     windowSeconds.data(),
                     windowSeconds.size(),
                     requestLimit.data(),
                     requestLimit.size())));
    if (isErrorReply(reply.get()) || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
        reply->element[0] == nullptr || reply->element[1] == nullptr ||
        reply->element[0]->type != REDIS_REPLY_INTEGER ||
        reply->element[1]->type != REDIS_REPLY_INTEGER)
    {
        LOG_ERROR << "rate_limit result=error fail_open=true stage=eval";
        return { Status::Error, 0 };
    }

    const bool allowed = reply->element[0]->integer == 1;
    const int retryAfterSeconds = std::max(1, static_cast<int>(reply->element[1]->integer));
    return { allowed ? Status::Allowed : Status::Limited, retryAfterSeconds };
}

} // namespace shortlink
