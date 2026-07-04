#pragma once

#include <optional>
#include <string>

namespace shortlink
{

class ShortLinkRepository
{
public:
    struct ShortLinkRecord
    {
        std::string code;
        std::string originalUrl;
    };

    virtual ~ShortLinkRepository() = default;

    virtual std::optional<ShortLinkRecord> create(const std::string& originalUrl) = 0;
    virtual std::optional<std::string> findOriginalUrl(const std::string& code) const = 0;
};

} // namespace shortlink
