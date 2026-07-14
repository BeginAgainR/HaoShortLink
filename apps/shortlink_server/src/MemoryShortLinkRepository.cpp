#include "shortlink/MemoryShortLinkRepository.h"

#include <algorithm>

namespace shortlink
{

namespace
{

const char kBase62Chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr std::size_t kBase62Size = 62;
constexpr std::size_t kMinCodeLength = 6;

} // namespace

std::optional<ShortLinkRepository::ShortLinkRecord>
MemoryShortLinkRepository::create(const std::string& originalUrl)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string code = encodeBase62(nextId_++);
    links_[code] = originalUrl;

    return ShortLinkRecord {
        code,
        originalUrl
    };
}

std::optional<std::string> MemoryShortLinkRepository::findOriginalUrl(const std::string& code) const
{
    try
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto it = links_.find(code);
        if (it == links_.end())
        {
            if (metrics_ != nullptr)
            {
                metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::NotFound,
                                         ShortLinkMetrics::RedirectSource::Memory);
            }
            return std::nullopt;
        }

        const std::string originalUrl = it->second;
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Success,
                                     ShortLinkMetrics::RedirectSource::Memory);
        }
        return originalUrl;
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Error,
                                     ShortLinkMetrics::RedirectSource::Memory);
        }
        throw;
    }
}

std::string MemoryShortLinkRepository::encodeBase62(std::uint64_t value)
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
