#include "shortlink/ShortLinkService.h"

#include <algorithm>

namespace shortlink
{

namespace
{

const char kBase62Chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr std::size_t kBase62Size = 62;
constexpr std::size_t kMinCodeLength = 6;

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

std::optional<ShortLinkService::ShortLink> ShortLinkService::createShortLink(const std::string& originalUrl)
{
    if (!isValidUrl(originalUrl))
    {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const std::string code = encodeBase62(nextId_++);
    const std::string shortUrl = "/s/" + code;
    links_[code] = originalUrl;

    return ShortLink {
        code,
        shortUrl,
        originalUrl
    };
}

std::optional<std::string> ShortLinkService::findOriginalUrl(const std::string& code) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = links_.find(code);
    if (it == links_.end())
    {
        return std::nullopt;
    }

    return it->second;
}

bool ShortLinkService::isValidUrl(const std::string& url) const
{
    return startsWith(url, "http://") || startsWith(url, "https://");
}

std::string ShortLinkService::encodeBase62(std::uint64_t value)
{
    if (value == 0)
    {
        return std::string(kMinCodeLength, '0');
    }

    std::string encoded;
    while (value > 0)
    {
        encoded.push_back(kBase62Chars[value % kBase62Size]);
        value /= kBase62Size;
    }

    while (encoded.size() < kMinCodeLength)
    {
        encoded.push_back('0');
    }

    std::reverse(encoded.begin(), encoded.end());
    return encoded;
}

} // namespace shortlink
