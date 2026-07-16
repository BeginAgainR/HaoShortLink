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

    std::optional<ShortLinkRecord> create(
        const std::string& originalUrl,
        std::optional<std::int64_t> expiresAt = std::nullopt) override;
    LookupResult findByCode(const std::string& code) const override;
    LookupSource defaultLookupSource() const noexcept override
    {
        return LookupSource::Memory;
    }
    std::vector<ShortLinkRecord> list(const ListQuery& query) const override;
    std::optional<ShortLinkRecord> updateLifecycle(
        const std::string& code,
        const LifecycleUpdate& update) override;

private:
    static std::string encodeBase62(std::uint64_t value);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ShortLinkRecord> links_;
    std::uint64_t nextId_ { 1 };
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
