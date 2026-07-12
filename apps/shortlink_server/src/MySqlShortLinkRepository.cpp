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
MySqlShortLinkRepository::create(const std::string& originalUrl)
{
    std::shared_ptr<http::db::DbConnection> conn;

    try
    {
        conn = http::db::DbConnectionPool::getInstance().getConnection();
        conn->executeRawUpdate("START TRANSACTION");
        conn->executeUpdate("INSERT INTO short_links (original_url) VALUES (?)", originalUrl);

        std::unique_ptr<sql::ResultSet> idResult(
            conn->executeQuery("SELECT LAST_INSERT_ID()"));
        if (!idResult || !idResult->next())
        {
            throw std::runtime_error("Failed to read last insert id");
        }

        const std::uint64_t id = static_cast<std::uint64_t>(idResult->getUInt64(1));
        const std::string code = encodeBase62(id);
        const std::string idString = std::to_string(id);

        conn->executeUpdate("UPDATE short_links SET code = ? WHERE id = ?",
                            code,
                            idString);
        conn->executeRawUpdate("COMMIT");

        return ShortLinkRecord {
            code,
            originalUrl
        };
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

std::optional<std::string> MySqlShortLinkRepository::findOriginalUrl(const std::string& code) const
{
    try
    {
        auto conn = http::db::DbConnectionPool::getInstance().getConnection();
        std::unique_ptr<sql::ResultSet> result(
            conn->executeQuery("SELECT original_url FROM short_links WHERE code = ? LIMIT 1",
                               code));

        if (!result || !result->next())
        {
            if (metrics_ != nullptr)
            {
                metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::NotFound,
                                         ShortLinkMetrics::RedirectSource::Mysql);
            }
            return std::nullopt;
        }

        const std::string originalUrl = result->getString("original_url");
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Success,
                                     ShortLinkMetrics::RedirectSource::Mysql);
        }
        return originalUrl;
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Error,
                                     ShortLinkMetrics::RedirectSource::Mysql);
            metrics_->recordBackendError(ShortLinkMetrics::Backend::Mysql,
                                         ShortLinkMetrics::BackendOperation::Find);
        }
        throw;
    }
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
