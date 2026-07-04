#pragma once

#include "shortlink/ShortLinkRepository.h"

#include <optional>
#include <string>

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

    explicit ShortLinkService(ShortLinkRepository& repository);

    std::optional<ShortLink> createShortLink(const std::string& originalUrl);
    std::optional<std::string> findOriginalUrl(const std::string& code) const;
    bool isValidUrl(const std::string& url) const;

private:
    ShortLinkRepository& repository_;
};

} // namespace shortlink
