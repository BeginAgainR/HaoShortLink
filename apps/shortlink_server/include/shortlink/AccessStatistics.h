#pragma once

#include "shortlink/AccessEvent.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace shortlink
{

constexpr std::size_t kAccessStatisticsResultCount = 4;

enum class AccessStatisticsInterval
{
    Hour,
    Day
};

using AccessStatisticsResultCounts =
    std::array<std::uint64_t, kAccessStatisticsResultCount>;

struct AccessStatisticsQuery
{
    std::int64_t fromEpochSeconds = 0;
    std::int64_t toEpochSeconds = 0;
    AccessStatisticsInterval interval = AccessStatisticsInterval::Day;
};

struct AccessStatisticsSummary
{
    AccessStatisticsResultCounts resultCounts {};
    std::optional<std::int64_t> lastAccessAtMs;
    std::optional<std::int64_t> lastAttemptAtMs;
};

struct AccessStatisticsTrendPoint
{
    std::int64_t bucketStartEpochSeconds = 0;
    AccessStatisticsResultCounts resultCounts {};
};

struct AccessStatisticsSnapshot
{
    std::string code;
    AccessStatisticsSummary summary;
    std::vector<AccessStatisticsTrendPoint> trend;
};

bool isAggregatedAccessResult(AccessEventResult result) noexcept;
std::optional<std::size_t> accessStatisticsResultIndex(AccessEventResult result) noexcept;
std::int64_t utcHourBucketStartEpochSeconds(std::int64_t occurredAtMs);
const char* accessStatisticsIntervalToString(AccessStatisticsInterval interval) noexcept;
std::optional<AccessStatisticsInterval> parseAccessStatisticsInterval(
    const std::string& value) noexcept;
std::int64_t accessStatisticsBucketSeconds(AccessStatisticsInterval interval) noexcept;
bool isValidAccessStatisticsQuery(const AccessStatisticsQuery& query) noexcept;
std::uint64_t accessStatisticsAttemptCount(
    const AccessStatisticsResultCounts& counts) noexcept;

} // namespace shortlink
