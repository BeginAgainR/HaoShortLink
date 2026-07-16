#pragma once

#include "shortlink/ShortLinkRepository.h"
#include "shortlink/ShortLinkMetrics.h"

#include <cstdint>
#include <string>

namespace sql
{
class ResultSet;
}

namespace shortlink
{

class MySqlShortLinkRepository : public ShortLinkRepository
{
public:
    explicit MySqlShortLinkRepository(ShortLinkMetrics* metrics = nullptr)
        : metrics_(metrics)
    {}

    std::optional<ShortLinkRecord> create(
        const std::string& originalUrl,
        std::optional<std::int64_t> expiresAt = std::nullopt) override;
    LookupResult findByCode(const std::string& code) const override;
    LookupSource defaultLookupSource() const noexcept override
    {
        return LookupSource::Mysql;
    }
    std::vector<ShortLinkRecord> list(const ListQuery& query) const override;
    std::optional<ShortLinkRecord> updateLifecycle(
        const std::string& code,
        const LifecycleUpdate& update) override;

private:
    static std::string encodeBase62(std::uint64_t value);
    static ShortLinkRecord readRecord(sql::ResultSet* result);

private:
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
