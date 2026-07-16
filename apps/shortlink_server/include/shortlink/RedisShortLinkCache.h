#pragma once

#include "shortlink/ShortLinkMetrics.h"
#include "shortlink/ShortLinkRepository.h"

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

    std::optional<ShortLinkRepository::ShortLinkRecord> get(const std::string& code) const;
    bool set(const ShortLinkRepository::ShortLinkRecord& record) const;
    bool erase(const std::string& code) const;

private:
    Config config_;
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
