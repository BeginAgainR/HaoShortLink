#pragma once

#include "shortlink/AccessStatistics.h"

#include <optional>
#include <string>

namespace shortlink
{

class AccessStatisticsRepository
{
public:
    virtual ~AccessStatisticsRepository() = default;

    virtual std::optional<AccessStatisticsSnapshot> get(
        const std::string& code,
        const AccessStatisticsQuery& query) const = 0;
};

} // namespace shortlink
