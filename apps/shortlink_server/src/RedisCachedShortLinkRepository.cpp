#include "shortlink/RedisCachedShortLinkRepository.h"

#include <utility>

namespace shortlink
{

RedisCachedShortLinkRepository::RedisCachedShortLinkRepository(
    ShortLinkRepository& sourceRepository,
    RedisShortLinkCache cache,
    ShortLinkMetrics* metrics)
    : sourceRepository_(sourceRepository),
      cache_(std::move(cache)),
      metrics_(metrics)
{}

std::optional<ShortLinkRepository::ShortLinkRecord>
RedisCachedShortLinkRepository::create(const std::string& originalUrl,
                                       std::optional<std::int64_t> expiresAt,
                                       std::uint64_t ownerId,
                                       std::optional<std::string> customCode)
{
    return sourceRepository_.create(originalUrl, expiresAt, ownerId, std::move(customCode));
}

ShortLinkRepository::LookupResult RedisCachedShortLinkRepository::findByCode(
    const std::string& code) const
{
    const std::optional<ShortLinkRecord> cached = cache_.get(code);
    if (cached)
    {
        return { cached, LookupSource::Redis };
    }

    LookupResult result = sourceRepository_.findByCode(code);
    if (result.record)
    {
        cache_.set(*result.record);
    }
    return result;
}

std::optional<ShortLinkRepository::ShortLinkRecord>
RedisCachedShortLinkRepository::findByCodeForOwner(const std::string& code,
                                                   std::uint64_t ownerId) const
{
    return sourceRepository_.findByCodeForOwner(code, ownerId);
}

std::vector<ShortLinkRepository::ShortLinkRecord> RedisCachedShortLinkRepository::list(
    const ListQuery& query) const
{
    return sourceRepository_.list(query);
}

std::optional<ShortLinkRepository::ShortLinkRecord>
RedisCachedShortLinkRepository::updateLifecycle(const std::string& code,
                                                const LifecycleUpdate& update)
{
    std::optional<ShortLinkRecord> record = sourceRepository_.updateLifecycle(code, update);
    if (record)
    {
        cache_.erase(code);
    }
    return record;
}

} // namespace shortlink
