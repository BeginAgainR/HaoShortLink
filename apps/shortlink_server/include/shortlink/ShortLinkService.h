#pragma once

#include "shortlink/ShortLinkRepository.h"

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

namespace shortlink
{

class ShortLinkMetrics;

class ShortLinkService
{
public:
    struct ShortLink
    {
        ShortLinkRepository::ShortLinkRecord record;
        std::string shortUrl;
    };

    enum class RedirectStatus
    {
        Success,
        NotFound,
        Disabled,
        Expired
    };

    struct RedirectResult
    {
        RedirectStatus status { RedirectStatus::NotFound };
        std::optional<std::string> originalUrl;
    };

    explicit ShortLinkService(ShortLinkRepository& repository,
                              ShortLinkMetrics* metrics = nullptr);

    std::optional<ShortLink> createShortLink(
        const std::string& originalUrl,
        std::optional<std::int64_t> expiresAt = std::nullopt);
    RedirectResult resolve(const std::string& code) const;
    std::optional<ShortLinkRepository::ShortLinkRecord> get(const std::string& code) const;
    std::vector<ShortLinkRepository::ShortLinkRecord> list(
        const ShortLinkRepository::ListQuery& query) const;
    std::optional<ShortLinkRepository::ShortLinkRecord> updateLifecycle(
        const std::string& code,
        const ShortLinkRepository::LifecycleUpdate& update);
    bool isValidUrl(const std::string& url) const;
    static bool isExpired(const ShortLinkRepository::ShortLinkRecord& record,
                          std::int64_t nowEpochSeconds);
    static std::optional<std::int64_t> parseUtcTimestamp(const std::string& value);
    static std::string formatUtcTimestamp(std::int64_t epochSeconds);
    static std::int64_t nowEpochSeconds();

private:
    ShortLinkRepository& repository_;
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
