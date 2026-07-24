#pragma once

#include "shortlink/RedisShortLinkCache.h"
#include "shortlink/ShortLinkRepository.h"

#include <optional>
#include <string>

namespace shortlink
{

class RedisCachedShortLinkRepository : public ShortLinkRepository
{
public:
    RedisCachedShortLinkRepository(ShortLinkRepository& sourceRepository,
                                   RedisShortLinkCache cache,
                                   ShortLinkMetrics* metrics = nullptr);

    std::optional<ShortLinkRecord> create(
        const std::string& originalUrl,
        std::optional<std::int64_t> expiresAt = std::nullopt,
        std::uint64_t ownerId = 1,
        std::optional<std::string> customCode = std::nullopt) override;
    LookupResult findByCode(const std::string& code) const override;
    std::optional<ShortLinkRecord> findByCodeForOwner(
        const std::string& code,
        std::uint64_t ownerId) const override;
    LookupSource defaultLookupSource() const noexcept override
    {
        return sourceRepository_.defaultLookupSource();
    }
    std::vector<ShortLinkRecord> list(const ListQuery& query) const override;
    std::optional<ShortLinkRecord> updateLifecycle(
        const std::string& code,
        const LifecycleUpdate& update) override;

private:
    ShortLinkRepository& sourceRepository_;
    RedisShortLinkCache cache_;
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
