#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace shortlink
{

class ShortLinkService
{
public:
    struct ShortLink
    {
        std::string code;
        std::string shortUrl;
        std::string originalUrl;
    };

    std::optional<ShortLink> createShortLink(const std::string& originalUrl);
    std::optional<std::string> findOriginalUrl(const std::string& code) const;
    bool isValidUrl(const std::string& url) const;

private:
    static std::string encodeBase62(std::uint64_t value);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> links_;
    std::uint64_t nextId_ { 1 };
};

} // namespace shortlink
