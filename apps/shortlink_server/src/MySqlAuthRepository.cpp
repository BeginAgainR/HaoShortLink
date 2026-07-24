#include "shortlink/MySqlAuthRepository.h"

#include "utils/db/DbConnectionPool.h"

#include <memory>
#include <stdexcept>

namespace shortlink
{

namespace
{

bool isDuplicateError(const std::exception& error)
{
    const std::string message = error.what();
    return message.find("Duplicate entry") != std::string::npos ||
           message.find("1062") != std::string::npos;
}

} // namespace

AuthRepository::CreateUserResult MySqlAuthRepository::createUser(
    const std::string& username,
    const std::string& normalizedUsername,
    const std::string& passwordHash)
{
    try
    {
        auto connection = http::db::DbConnectionPool::getInstance().getConnection();
        connection->executeUpdate(
            "INSERT INTO users (username, username_normalized, password_hash) VALUES (?, ?, ?)",
            username,
            normalizedUsername,
            passwordHash);
        std::unique_ptr<sql::ResultSet> result(connection->executeQuery(
            "SELECT id, username, status, UNIX_TIMESTAMP(created_at) AS created_at_epoch, "
            "UNIX_TIMESTAMP(updated_at) AS updated_at_epoch "
            "FROM users WHERE id = LAST_INSERT_ID() LIMIT 1"));
        if (!result || !result->next())
        {
            throw std::runtime_error("Failed to read created user");
        }
        return { CreateUserStatus::Created, readUser(result.get()) };
    }
    catch (const std::exception& error)
    {
        if (isDuplicateError(error))
        {
            return { CreateUserStatus::Conflict, std::nullopt };
        }
        throw;
    }
}

std::optional<AuthRepository::Credentials> MySqlAuthRepository::findCredentials(
    const std::string& normalizedUsername) const
{
    auto connection = http::db::DbConnectionPool::getInstance().getConnection();
    std::unique_ptr<sql::ResultSet> result(connection->executeQuery(
        "SELECT id, username, password_hash, status, "
        "UNIX_TIMESTAMP(created_at) AS created_at_epoch, "
        "UNIX_TIMESTAMP(updated_at) AS updated_at_epoch "
        "FROM users WHERE username_normalized = ? LIMIT 1",
        normalizedUsername));
    if (!result || !result->next())
    {
        return std::nullopt;
    }
    return Credentials { readUser(result.get()), result->getString("password_hash") };
}

void MySqlAuthRepository::createSession(std::uint64_t userId,
                                        const std::string& tokenHash,
                                        std::int64_t expiresAt)
{
    auto connection = http::db::DbConnectionPool::getInstance().getConnection();
    connection->executeUpdate(
        "INSERT INTO user_sessions (token_hash, user_id, expires_at) "
        "VALUES (?, ?, DATE_ADD('1970-01-01 00:00:00', INTERVAL ? SECOND))",
        tokenHash,
        userId,
        expiresAt);
}

std::optional<AuthRepository::User> MySqlAuthRepository::findUserBySession(
    const std::string& tokenHash,
    std::int64_t nowEpochSeconds) const
{
    auto connection = http::db::DbConnectionPool::getInstance().getConnection();
    std::unique_ptr<sql::ResultSet> result(connection->executeQuery(
        "SELECT u.id, u.username, u.status, "
        "UNIX_TIMESTAMP(u.created_at) AS created_at_epoch, "
        "UNIX_TIMESTAMP(u.updated_at) AS updated_at_epoch "
        "FROM user_sessions s JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = ? AND s.revoked_at IS NULL "
        "AND s.expires_at > DATE_ADD('1970-01-01 00:00:00', INTERVAL ? SECOND) "
        "AND u.status = 'active' LIMIT 1",
        tokenHash,
        nowEpochSeconds));
    if (!result || !result->next())
    {
        return std::nullopt;
    }
    return readUser(result.get());
}

void MySqlAuthRepository::revokeSession(const std::string& tokenHash,
                                        std::int64_t revokedAt)
{
    auto connection = http::db::DbConnectionPool::getInstance().getConnection();
    connection->executeUpdate(
        "UPDATE user_sessions SET revoked_at = "
        "DATE_ADD('1970-01-01 00:00:00', INTERVAL ? SECOND) "
        "WHERE token_hash = ? AND revoked_at IS NULL",
        revokedAt,
        tokenHash);
}

AuthRepository::User MySqlAuthRepository::readUser(sql::ResultSet* result)
{
    User user;
    user.id = result->getUInt64("id");
    user.username = result->getString("username");
    const std::string status = result->getString("status");
    if (status == "active")
    {
        user.status = UserStatus::Active;
    }
    else if (status == "disabled")
    {
        user.status = UserStatus::Disabled;
    }
    else
    {
        throw std::runtime_error("Unknown user status");
    }
    user.createdAt = result->getInt64("created_at_epoch");
    user.updatedAt = result->getInt64("updated_at_epoch");
    return user;
}

} // namespace shortlink
