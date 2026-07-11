#include "shortlink/RedisCachedShortLinkRepository.h"

#include <utility>

namespace shortlink
{

RedisCachedShortLinkRepository::RedisCachedShortLinkRepository(
    ShortLinkRepository& sourceRepository,
    RedisShortLinkCache cache)
    : sourceRepository_(sourceRepository),
      cache_(std::move(cache))
{}

std::optional<ShortLinkRepository::ShortLinkRecord>
RedisCachedShortLinkRepository::create(const std::string& originalUrl)
{
    return sourceRepository_.create(originalUrl);
}

std::optional<std::string> RedisCachedShortLinkRepository::findOriginalUrl(const std::string& code) const
{
    std::optional<std::string> cachedOriginalUrl = cache_.getOriginalUrl(code);
    if (cachedOriginalUrl)
    {
        return cachedOriginalUrl;
    }

    std::optional<std::string> originalUrl = sourceRepository_.findOriginalUrl(code);
    if (originalUrl)
    {
        cache_.setOriginalUrl(code, *originalUrl);
    }

    return originalUrl;
}

} // namespace shortlink
