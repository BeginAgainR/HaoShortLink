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
RedisCachedShortLinkRepository::create(const std::string& originalUrl)
{
    return sourceRepository_.create(originalUrl);
}

std::optional<std::string> RedisCachedShortLinkRepository::findOriginalUrl(const std::string& code) const
{
    std::optional<std::string> cachedOriginalUrl;
    try
    {
        cachedOriginalUrl = cache_.getOriginalUrl(code);
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Error,
                                     ShortLinkMetrics::RedirectSource::Redis);
        }
        throw;
    }
    if (cachedOriginalUrl)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Success,
                                     ShortLinkMetrics::RedirectSource::Redis);
        }
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
