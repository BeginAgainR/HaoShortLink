#pragma once

#include "shortlink/ShortLinkRepository.h"
#include "shortlink/ShortLinkMetrics.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace shortlink
{

class MemoryShortLinkRepository : public ShortLinkRepository
{
public:
    explicit MemoryShortLinkRepository(ShortLinkMetrics* metrics = nullptr)
        : metrics_(metrics)
    {}

    std::optional<ShortLinkRecord> create(const std::string& originalUrl) override;
    std::optional<std::string> findOriginalUrl(const std::string& code) const override;

private:
    static std::string encodeBase62(std::uint64_t value);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> links_;
    std::uint64_t nextId_ { 1 };
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
