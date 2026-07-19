#include "shortlink/MySqlAccessStatisticsWriter.h"

#include "shortlink/AccessStatistics.h"
#include "utils/db/DbConnection.h"

#include <memory>
#include <stdexcept>

#include <cppconn/resultset.h>
#include <muduo/base/Logging.h>

namespace shortlink
{
namespace consumer
{

namespace
{

void validateConfig(const MySqlAccessStatisticsWriter::Config& config)
{
    if (config.host.empty() || config.user.empty() || config.database.empty())
    {
        throw std::invalid_argument("invalid MySQL access statistics writer config");
    }
}

void validateSource(const AccessEventSourcePosition& source)
{
    if (source.topic.empty() || source.topic.size() > 249 ||
        source.partition < 0 || source.offset < 0)
    {
        throw std::invalid_argument("invalid Kafka source position");
    }
}

} // namespace

MySqlAccessStatisticsWriter::MySqlAccessStatisticsWriter(Config config)
    : config_(std::move(config))
{
    validateConfig(config_);
    connection_ = std::make_unique<http::db::DbConnection>(config_.host,
                                                            config_.user,
                                                            config_.password,
                                                            config_.database);
}

MySqlAccessStatisticsWriter::~MySqlAccessStatisticsWriter() = default;

AccessStatisticsWriteResult MySqlAccessStatisticsWriter::process(
    const AccessEvent& event,
    const AccessEventSourcePosition& source)
{
    if (!isValidAccessEvent(event))
    {
        throw std::invalid_argument("cannot persist invalid access event");
    }
    validateSource(source);

    connection_->executeRawUpdate("START TRANSACTION");
    try
    {
        const int inserted = connection_->executeUpdate(
            "INSERT IGNORE INTO processed_access_events "
            "(event_id, source_topic, source_partition, source_offset, occurred_at_ms, "
            "disposition) VALUES (?, ?, ?, ?, ?, 'received')",
            event.eventId,
            source.topic,
            source.partition,
            source.offset,
            event.occurredAtMs);
        if (inserted == 0)
        {
            connection_->executeRawUpdate("COMMIT");
            return AccessStatisticsWriteResult::Duplicate;
        }

        if (event.result == AccessEventResult::NotFound)
        {
            connection_->executeUpdate(
                "UPDATE processed_access_events SET disposition = 'not_found_ignored' "
                "WHERE event_id = ?",
                event.eventId);
            connection_->executeRawUpdate("COMMIT");
            return AccessStatisticsWriteResult::IgnoredNotFound;
        }

        if (!isAggregatedAccessResult(event.result))
        {
            throw std::invalid_argument("unsupported access statistics result");
        }

        std::unique_ptr<sql::ResultSet> shortLinkResult(connection_->executeQuery(
            "SELECT id FROM short_links WHERE code = ? LIMIT 1",
            event.code));
        if (!shortLinkResult || !shortLinkResult->next())
        {
            throw OrphanShortLinkEvent(event.code);
        }

        const std::uint64_t shortLinkId = shortLinkResult->getUInt64("id");
        const std::string result = accessEventResultToString(event.result);
        const std::int64_t bucketStart = utcHourBucketStartEpochSeconds(event.occurredAtMs);

        connection_->executeUpdate(
            "INSERT INTO short_link_access_totals "
            "(short_link_id, result, access_count, first_occurred_at_ms, last_occurred_at_ms) "
            "VALUES (?, ?, 1, ?, ?) "
            "ON DUPLICATE KEY UPDATE "
            "access_count = access_count + 1, "
            "first_occurred_at_ms = LEAST(first_occurred_at_ms, VALUES(first_occurred_at_ms)), "
            "last_occurred_at_ms = GREATEST(last_occurred_at_ms, VALUES(last_occurred_at_ms))",
            shortLinkId,
            result,
            event.occurredAtMs,
            event.occurredAtMs);

        connection_->executeUpdate(
            "INSERT INTO short_link_access_hourly "
            "(short_link_id, bucket_start_epoch, result, access_count) "
            "VALUES (?, ?, ?, 1) "
            "ON DUPLICATE KEY UPDATE access_count = access_count + 1",
            shortLinkId,
            bucketStart,
            result);

        connection_->executeUpdate(
            "UPDATE processed_access_events SET disposition = 'aggregated' WHERE event_id = ?",
            event.eventId);
        connection_->executeRawUpdate("COMMIT");
        return AccessStatisticsWriteResult::Aggregated;
    }
    catch (...)
    {
        try
        {
            connection_->executeRawUpdate("ROLLBACK");
        }
        catch (const std::exception& error)
        {
            LOG_WARN << "event=access_statistics_writer stage=rollback result=failure"
                     << " reason=" << error.what();
        }
        catch (...)
        {
            LOG_WARN << "event=access_statistics_writer stage=rollback result=failure"
                     << " reason=unknown";
        }
        throw;
    }
}

void MySqlAccessStatisticsWriter::reconnect()
{
    connection_->reconnect();
}

} // namespace consumer
} // namespace shortlink
