#include "shortlink/AccessStatistics.h"

#include <numeric>
#include <stdexcept>

namespace shortlink
{

bool isAggregatedAccessResult(AccessEventResult result) noexcept
{
    return result == AccessEventResult::Success ||
           result == AccessEventResult::Disabled ||
           result == AccessEventResult::Expired ||
           result == AccessEventResult::Error;
}

std::optional<std::size_t> accessStatisticsResultIndex(AccessEventResult result) noexcept
{
    switch (result)
    {
    case AccessEventResult::Success:
        return 0;
    case AccessEventResult::Disabled:
        return 1;
    case AccessEventResult::Expired:
        return 2;
    case AccessEventResult::Error:
        return 3;
    case AccessEventResult::NotFound:
        return std::nullopt;
    }
    return std::nullopt;
}

std::int64_t utcHourBucketStartEpochSeconds(std::int64_t occurredAtMs)
{
    if (occurredAtMs <= 0)
    {
        throw std::invalid_argument("access event timestamp must be positive");
    }

    constexpr std::int64_t kSecondsPerHour = 3600;
    const std::int64_t occurredAtSeconds = occurredAtMs / 1000;
    return occurredAtSeconds - occurredAtSeconds % kSecondsPerHour;
}

const char* accessStatisticsIntervalToString(AccessStatisticsInterval interval) noexcept
{
    return interval == AccessStatisticsInterval::Hour ? "hour" : "day";
}

std::optional<AccessStatisticsInterval> parseAccessStatisticsInterval(
    const std::string& value) noexcept
{
    if (value == "hour")
    {
        return AccessStatisticsInterval::Hour;
    }
    if (value == "day")
    {
        return AccessStatisticsInterval::Day;
    }
    return std::nullopt;
}

std::int64_t accessStatisticsBucketSeconds(AccessStatisticsInterval interval) noexcept
{
    constexpr std::int64_t kSecondsPerHour = 3600;
    constexpr std::int64_t kHoursPerDay = 24;
    return interval == AccessStatisticsInterval::Hour
               ? kSecondsPerHour
               : kSecondsPerHour * kHoursPerDay;
}

bool isValidAccessStatisticsQuery(const AccessStatisticsQuery& query) noexcept
{
    if (query.fromEpochSeconds < 0 || query.toEpochSeconds <= query.fromEpochSeconds)
    {
        return false;
    }

    const std::int64_t bucketSeconds = accessStatisticsBucketSeconds(query.interval);
    if (query.fromEpochSeconds % bucketSeconds != 0 ||
        query.toEpochSeconds % bucketSeconds != 0)
    {
        return false;
    }

    constexpr std::int64_t kSecondsPerDay = 86400;
    const std::int64_t maxRange =
        query.interval == AccessStatisticsInterval::Hour
            ? 31 * kSecondsPerDay
            : 366 * kSecondsPerDay;
    return query.toEpochSeconds - query.fromEpochSeconds <= maxRange;
}

std::uint64_t accessStatisticsAttemptCount(
    const AccessStatisticsResultCounts& counts) noexcept
{
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t { 0 });
}

} // namespace shortlink
