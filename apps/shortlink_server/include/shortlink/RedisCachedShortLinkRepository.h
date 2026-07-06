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
                                   RedisShortLinkCache cache);

    std::optional<ShortLinkRecord> create(const std::string& originalUrl) override;
    std::optional<std::string> findOriginalUrl(const std::string& code) const override;

private:
    ShortLinkRepository& sourceRepository_;
    RedisShortLinkCache cache_;
};

} // namespace shortlink
