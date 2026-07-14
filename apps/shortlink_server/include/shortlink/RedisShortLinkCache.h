#pragma once

#include "shortlink/ShortLinkMetrics.h"

#include <optional>
#include <string>

namespace shortlink
{

class RedisShortLinkCache
{
public:
    struct Config
    {
        std::string host = "127.0.0.1";
        int port = 6379;
        int database = 0;
        int ttlSeconds = 3600;
        std::string keyPrefix = "shortlink:";
    };

    explicit RedisShortLinkCache(Config config, ShortLinkMetrics* metrics = nullptr);

    std::optional<std::string> getOriginalUrl(const std::string& code) const;
    bool setOriginalUrl(const std::string& code, const std::string& originalUrl) const;

private:
    Config config_;
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
