#include "shortlink/MySqlShortLinkRepository.h"

#include "utils/db/DbConnectionPool.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>

#include <muduo/base/Logging.h>

namespace shortlink
{

namespace
{

const char kBase62Chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr std::size_t kBase62Size = 62;
constexpr std::size_t kMinCodeLength = 6;

} // namespace

std::optional<ShortLinkRepository::ShortLinkRecord>
MySqlShortLinkRepository::create(const std::string& originalUrl,
                                 std::optional<std::int64_t> expiresAt,
                                 std::uint64_t ownerId,
                                 std::optional<std::string> customCode)
{
    std::shared_ptr<http::db::DbConnection> conn;
    const std::string requestedCode = customCode.value_or("");

    try
    {
        conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->executeRawUpdate("START TRANSACTION");
        if (customCode && expiresAt)
        {
            conn->executeUpdate(
                "INSERT INTO short_links (owner_id, code, original_url, expires_at) "
                "VALUES (?, ?, ?, DATE_ADD('1970-01-01 00:00:00', INTERVAL ? SECOND))",
                ownerId,
                requestedCode,
                originalUrl,
                *expiresAt);
        }
        else if (customCode)
        {
            conn->executeUpdate(
                "INSERT INTO short_links (owner_id, code, original_url) VALUES (?, ?, ?)",
                ownerId,
                requestedCode,
                originalUrl);
        }
        else if (expiresAt)
        {
            conn->executeUpdate(
                "INSERT INTO short_links (owner_id, original_url, expires_at) "
                "VALUES (?, ?, DATE_ADD('1970-01-01 00:00:00', INTERVAL ? SECOND))",
                ownerId,
                originalUrl,
                *expiresAt);
        }
        else
        {
            conn->executeUpdate(
                "INSERT INTO short_links (owner_id, original_url) VALUES (?, ?)",
                ownerId,
                originalUrl);
        }

        std::unique_ptr<sql::ResultSet> idResult(
            conn->executeQuery("SELECT LAST_INSERT_ID()"));
        if (!idResult || !idResult->next())
        {
            throw std::runtime_error("Failed to read last insert id");
        }

        const std::uint64_t id = static_cast<std::uint64_t>(idResult->getUInt64(1));
        const std::string code = customCode ? requestedCode : encodeBase62(id);
        const std::string idString = std::to_string(id);

        if (!customCode)
        {
            conn->executeUpdate("UPDATE short_links SET code = ? WHERE id = ?",
                                code,
                                idString);
        }

        std::unique_ptr<sql::ResultSet> recordResult(conn->executeQuery(
            "SELECT id, owner_id, code, original_url, status, "
            "TIMESTAMPDIFF(SECOND, '1970-01-01 00:00:00', expires_at) AS expires_at_epoch, "
            "UNIX_TIMESTAMP(created_at) AS created_at_epoch, "
            "UNIX_TIMESTAMP(updated_at) AS updated_at_epoch "
            "FROM short_links WHERE id = ? LIMIT 1",
            idString));
        if (!recordResult || !recordResult->next())
        {
            throw std::runtime_error("Failed to read created short link");
        }
        const ShortLinkRecord record = readRecord(recordResult.get());
        conn->executeRawUpdate("COMMIT");
        return record;
    }
    catch (const std::exception& e)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::Create);
        }
        LOG_ERROR << "Failed to create short link in MySQL: " << e.what();
        if (conn)
        {
            try
            {
                conn->executeRawUpdate("ROLLBACK");
            }
            catch (...)
            {
            }
        }
        const std::string message = e.what();
        if (customCode && (message.find("Duplicate entry") != std::string::npos ||
                           message.find("1062") != std::string::npos))
        {
            throw ShortCodeConflict();
        }
        throw;
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::Create);
        }
        LOG_ERROR << "Failed to create short link in MySQL: unknown error";
        if (conn)
        {
            try
            {
                conn->executeRawUpdate("ROLLBACK");
            }
            catch (...)
            {
            }
        }
        throw;
    }
}

ShortLinkRepository::LookupResult MySqlShortLinkRepository::findByCode(
    const std::string& code) const
{
    try
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> result(
            conn->executeQuery(
                               "SELECT id, owner_id, code, original_url, status, "
                               "TIMESTAMPDIFF(SECOND, '1970-01-01 00:00:00', expires_at) "
                               "AS expires_at_epoch, "
                               "UNIX_TIMESTAMP(created_at) AS created_at_epoch, "
                               "UNIX_TIMESTAMP(updated_at) AS updated_at_epoch "
                               "FROM short_links WHERE code = ? LIMIT 1",
                               code));

        if (!result || !result->next())
        {
            return { std::nullopt, LookupSource::Mysql };
        }
        return { readRecord(result.get()), LookupSource::Mysql };
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::Find);
        }
        throw;
    }
}

std::optional<ShortLinkRepository::ShortLinkRecord>
MySqlShortLinkRepository::findByCodeForOwner(const std::string& code,
                                             std::uint64_t ownerId) const
{
    try
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> result(conn->executeQuery(
            "SELECT id, owner_id, code, original_url, status, "
            "TIMESTAMPDIFF(SECOND, '1970-01-01 00:00:00', expires_at) "
            "AS expires_at_epoch, "
            "UNIX_TIMESTAMP(created_at) AS created_at_epoch, "
            "UNIX_TIMESTAMP(updated_at) AS updated_at_epoch "
            "FROM short_links WHERE code = ? AND owner_id = ? LIMIT 1",
            code,
            ownerId));
        if (!result || !result->next())
        {
            return std::nullopt;
        }
        return readRecord(result.get());
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::Find);
        }
        throw;
    }
}

std::vector<ShortLinkRepository::ShortLinkRecord> MySqlShortLinkRepository::list(
    const ListQuery& query) const
{
    try
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> result;
        const std::string fields =
            "SELECT id, owner_id, code, original_url, status, "
            "TIMESTAMPDIFF(SECOND, '1970-01-01 00:00:00', expires_at) AS expires_at_epoch, "
            "UNIX_TIMESTAMP(created_at) AS created_at_epoch, "
            "UNIX_TIMESTAMP(updated_at) AS updated_at_epoch FROM short_links ";
        if (query.ownerId && query.status)
        {
            const std::string statusValue = statusToString(*query.status);
            result.reset(conn->executeQuery(
                fields + "WHERE owner_id = ? AND id > ? AND status = ? ORDER BY id ASC LIMIT ?",
                *query.ownerId,
                query.cursor,
                statusValue,
                query.limit));
        }
        else if (query.ownerId)
        {
            result.reset(conn->executeQuery(
                fields + "WHERE owner_id = ? AND id > ? ORDER BY id ASC LIMIT ?",
                *query.ownerId,
                query.cursor,
                query.limit));
        }
        else if (query.status)
        {
            const std::string statusValue = statusToString(*query.status);
            result.reset(conn->executeQuery(
                fields + "WHERE id > ? AND status = ? ORDER BY id ASC LIMIT ?",
                query.cursor,
                statusValue,
                query.limit));
        }
        else
        {
            result.reset(conn->executeQuery(
                fields + "WHERE id > ? ORDER BY id ASC LIMIT ?", query.cursor, query.limit));
        }

        std::vector<ShortLinkRecord> records;
        while (result && result->next())
        {
            records.push_back(readRecord(result.get()));
        }
        return records;
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::List);
        }
        throw;
    }
}

std::optional<ShortLinkRepository::ShortLinkRecord> MySqlShortLinkRepository::updateLifecycle(
    const std::string& code,
    const LifecycleUpdate& update)
{
    try
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        const std::string statusValue = update.status ? statusToString(*update.status) : "";
        const std::uint64_t ownerId = update.ownerId.value_or(0);
        if (update.status && update.expiresAtProvided && update.expiresAt)
        {
            conn->executeUpdate(
                "UPDATE short_links SET status = ?, "
                "expires_at = DATE_ADD('1970-01-01 00:00:00', INTERVAL ? SECOND) "
                "WHERE code = ? AND (? = 0 OR owner_id = ?)",
                statusValue,
                *update.expiresAt,
                code,
                ownerId,
                ownerId);
        }
        else if (update.status && update.expiresAtProvided)
        {
            conn->executeUpdate(
                "UPDATE short_links SET status = ?, expires_at = NULL "
                "WHERE code = ? AND (? = 0 OR owner_id = ?)",
                statusValue,
                code,
                ownerId,
                ownerId);
        }
        else if (update.status)
        {
            conn->executeUpdate(
                "UPDATE short_links SET status = ? "
                "WHERE code = ? AND (? = 0 OR owner_id = ?)",
                statusValue,
                code,
                ownerId,
                ownerId);
        }
        else if (update.expiresAtProvided && update.expiresAt)
        {
            conn->executeUpdate(
                "UPDATE short_links SET "
                "expires_at = DATE_ADD('1970-01-01 00:00:00', INTERVAL ? SECOND) "
                "WHERE code = ? AND (? = 0 OR owner_id = ?)",
                *update.expiresAt,
                code,
                ownerId,
                ownerId);
        }
        else if (update.expiresAtProvided)
        {
            conn->executeUpdate(
                "UPDATE short_links SET expires_at = NULL "
                "WHERE code = ? AND (? = 0 OR owner_id = ?)",
                code,
                ownerId,
                ownerId);
        }

        std::unique_ptr<sql::ResultSet> result(conn->executeQuery(
            "SELECT id, owner_id, code, original_url, status, "
            "TIMESTAMPDIFF(SECOND, '1970-01-01 00:00:00', expires_at) AS expires_at_epoch, "
            "UNIX_TIMESTAMP(created_at) AS created_at_epoch, "
            "UNIX_TIMESTAMP(updated_at) AS updated_at_epoch "
            "FROM short_links WHERE code = ? AND (? = 0 OR owner_id = ?) LIMIT 1",
            code,
            ownerId,
            ownerId));
        if (!result || !result->next())
        {
            return std::nullopt;
        }
        return readRecord(result.get());
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::Update);
        }
        throw;
    }
}

ShortLinkRepository::ShortLinkRecord MySqlShortLinkRepository::readRecord(sql::ResultSet* result)
{
    const std::optional<Status> status = parseStatus(result->getString("status"));
    if (!status)
    {
        throw std::runtime_error("Unknown short link status");
    }

    ShortLinkRecord record;
    record.id = result->getUInt64("id");
    record.ownerId = result->getUInt64("owner_id");
    record.code = result->getString("code");
    record.originalUrl = result->getString("original_url");
    record.status = *status;
    if (!result->isNull("expires_at_epoch"))
    {
        record.expiresAt = result->getInt64("expires_at_epoch");
    }
    record.createdAt = result->getInt64("created_at_epoch");
    record.updatedAt = result->getInt64("updated_at_epoch");
    return record;
}

std::string MySqlShortLinkRepository::encodeBase62(std::uint64_t value)
{
    if (value == 0)
    {
        return std::string(kMinCodeLength, '0');
    }

    std::string encoded;
    while (value > 0)
    {
        encoded.push_back(kBase62Chars[value % kBase62Size]);
        value /= kBase62Size;
    }

    while (encoded.size() < kMinCodeLength)
    {
        encoded.push_back('0');
    }

    std::reverse(encoded.begin(), encoded.end());
    return encoded;
}

} // namespace shortlink
