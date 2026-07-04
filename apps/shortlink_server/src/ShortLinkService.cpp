#include "shortlink/ShortLinkService.h"

namespace shortlink
{

namespace
{

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

ShortLinkService::ShortLinkService(ShortLinkRepository& repository)
    : repository_(repository)
{}

std::optional<ShortLinkService::ShortLink> ShortLinkService::createShortLink(const std::string& originalUrl)
{
    if (!isValidUrl(originalUrl))
    {
        return std::nullopt;
    }

    const std::optional<ShortLinkRepository::ShortLinkRecord> record =
        repository_.create(originalUrl);
    if (!record)
    {
        return std::nullopt;
    }

    return ShortLink {
        record->code,
        "/s/" + record->code,
        record->originalUrl
    };
}

std::optional<std::string> ShortLinkService::findOriginalUrl(const std::string& code) const
{
    return repository_.findOriginalUrl(code);
}

bool ShortLinkService::isValidUrl(const std::string& url) const
{
    return startsWith(url, "http://") || startsWith(url, "https://");
}

} // namespace shortlink
