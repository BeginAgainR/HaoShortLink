#include "shortlink/MySqlAccessStatisticsRepository.h"

#include "shortlink/AccessEvent.h"
#include "shortlink/ShortLinkMetrics.h"
#include "utils/db/DbConnectionPool.h"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>

#include <cppconn/resultset.h>

namespace shortlink
{
namespace
{

std::size_t resultIndex(const std::string& value)
{
    const std::optional<AccessEventResult> result = parseAccessEventResult(value);
    if (!result)
    {
        throw std::runtime_error("unknown access statistics result");
    }
    const std::optional<std::size_t> index = accessStatisticsResultIndex(*result);
    if (!index)
    {
        throw std::runtime_error("non-aggregated access statistics result");
    }
    return *index;
}

} // namespace

std::optional<AccessStatisticsSnapshot> MySqlAccessStatisticsRepository::get(
    const std::string& code,
    const AccessStatisticsQuery& query) const
{
    if (!isValidAccessStatisticsQuery(query))
    {
        throw std::invalid_argument("invalid access statistics query");
    }

    try
    {
        auto connection = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> linkResult(connection->executeQuery(
            "SELECT id FROM short_links WHERE code = ? LIMIT 1", code));
        if (!linkResult || !linkResult->next())
        {
            return std::nullopt;
        }

        const std::uint64_t shortLinkId = linkResult->getUInt64("id");
        AccessStatisticsSnapshot snapshot;
        snapshot.code = code;

        std::unique_ptr<sql::ResultSet> totalsResult(connection->executeQuery(
            "SELECT result, access_count, last_occurred_at_ms "
            "FROM short_link_access_totals WHERE short_link_id = ?",
            shortLinkId));
        while (totalsResult && totalsResult->next())
        {
            const std::size_t index = resultIndex(totalsResult->getString("result"));
            snapshot.summary.resultCounts[index] = totalsResult->getUInt64("access_count");
            const std::int64_t lastOccurredAtMs =
                totalsResult->getInt64("last_occurred_at_ms");
            if (index == 0)
            {
                snapshot.summary.lastAccessAtMs = lastOccurredAtMs;
            }
            if (!snapshot.summary.lastAttemptAtMs ||
                lastOccurredAtMs > *snapshot.summary.lastAttemptAtMs)
            {
                snapshot.summary.lastAttemptAtMs = lastOccurredAtMs;
            }
        }

        const std::string bucketExpression =
            query.interval == AccessStatisticsInterval::Hour
                ? "bucket_start_epoch"
                : "bucket_start_epoch - MOD(bucket_start_epoch, 86400)";
        std::unique_ptr<sql::ResultSet> trendResult(connection->executeQuery(
            "SELECT " + bucketExpression + " AS bucket_start, result, "
            "SUM(access_count) AS result_count "
            "FROM short_link_access_hourly "
            "WHERE short_link_id = ? AND bucket_start_epoch >= ? "
            "AND bucket_start_epoch < ? "
            "GROUP BY bucket_start, result ORDER BY bucket_start ASC",
            shortLinkId,
            query.fromEpochSeconds,
            query.toEpochSeconds));

        std::map<std::int64_t, AccessStatisticsResultCounts> trend;
        while (trendResult && trendResult->next())
        {
            const std::int64_t bucketStart = trendResult->getInt64("bucket_start");
            const std::size_t index = resultIndex(trendResult->getString("result"));
            trend[bucketStart][index] = trendResult->getUInt64("result_count");
        }
        snapshot.trend.reserve(trend.size());
        for (const auto& item : trend)
        {
            snapshot.trend.push_back({ item.first, item.second });
        }
        return snapshot;
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::Statistics);
        }
        throw;
    }
}

} // namespace shortlink
