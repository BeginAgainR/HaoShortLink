#pragma once

#include "shortlink/AccessStatisticsRepository.h"

namespace shortlink
{

class ShortLinkMetrics;

class MySqlAccessStatisticsRepository final : public AccessStatisticsRepository
{
public:
    explicit MySqlAccessStatisticsRepository(ShortLinkMetrics* metrics = nullptr)
        : metrics_(metrics)
    {
    }

    std::optional<AccessStatisticsSnapshot> get(
        const std::string& code,
        const AccessStatisticsQuery& query) const override;

private:
    ShortLinkMetrics* metrics_;
};

} // namespace shortlink
