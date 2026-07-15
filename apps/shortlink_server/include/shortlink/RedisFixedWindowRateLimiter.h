#pragma once

#include "shortlink/RateLimiter.h"

#include <string>

namespace shortlink
{

class RedisFixedWindowRateLimiter : public RateLimiter
{
public:
    struct Config
    {
        std::string host = "127.0.0.1";
        int port = 6379;
        int database = 0;
        int requests = 100;
        int windowSeconds = 60;
        std::string keyPrefix = "rate-limit:create:";
    };

    explicit RedisFixedWindowRateLimiter(Config config);

    Result check(const std::string& key) const override;

private:
    Config config_;
};

} // namespace shortlink
