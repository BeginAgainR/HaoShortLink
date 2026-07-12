#pragma once

#include "shortlink/ShortLinkRepository.h"
#include "shortlink/ShortLinkMetrics.h"

#include <cstdint>
#include <string>

namespace shortlink
{

class MySqlShortLinkRepository : public ShortLinkRepository
{
public:
    explicit MySqlShortLinkRepository(ShortLinkMetrics* metrics = nullptr)
        : metrics_(metrics)
    {}

    std::optional<ShortLinkRecord> create(const std::string& originalUrl) override;
    std::optional<std::string> findOriginalUrl(const std::string& code) const override;

private:
    static std::string encodeBase62(std::uint64_t value);

private:
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
