#include "shortlink/MemoryAuthRepository.h"

#include <chrono>

namespace shortlink
{

AuthRepository::CreateUserResult MemoryAuthRepository::createUser(
    const std::string& username,
    const std::string& normalizedUsername,
    const std::string& passwordHash)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (userIdsByName_.find(normalizedUsername) != userIdsByName_.end())
    {
        return { CreateUserStatus::Conflict, std::nullopt };
    }

    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    User user;
    user.id = nextUserId_++;
    user.username = username;
    user.status = UserStatus::Active;
    user.createdAt = now;
    user.updatedAt = now;
    users_[user.id] = { user, normalizedUsername, passwordHash };
    userIdsByName_[normalizedUsername] = user.id;
    return { CreateUserStatus::Created, user };
}

std::optional<AuthRepository::Credentials> MemoryAuthRepository::findCredentials(
    const std::string& normalizedUsername) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = userIdsByName_.find(normalizedUsername);
    if (id == userIdsByName_.end())
    {
        return std::nullopt;
    }
    const auto user = users_.find(id->second);
    if (user == users_.end())
    {
        return std::nullopt;
    }
    return Credentials { user->second.user, user->second.passwordHash };
}

void MemoryAuthRepository::createSession(std::uint64_t userId,
                                         const std::string& tokenHash,
                                         std::int64_t expiresAt)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[tokenHash] = { userId, expiresAt, std::nullopt };
}

std::optional<AuthRepository::User> MemoryAuthRepository::findUserBySession(
    const std::string& tokenHash,
    std::int64_t nowEpochSeconds) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = sessions_.find(tokenHash);
    if (session == sessions_.end() || session->second.revokedAt ||
        session->second.expiresAt <= nowEpochSeconds)
    {
        return std::nullopt;
    }
    const auto user = users_.find(session->second.userId);
    if (user == users_.end() || user->second.user.status != UserStatus::Active)
    {
        return std::nullopt;
    }
    return user->second.user;
}

void MemoryAuthRepository::revokeSession(const std::string& tokenHash,
                                         std::int64_t revokedAt)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = sessions_.find(tokenHash);
    if (session != sessions_.end())
    {
        session->second.revokedAt = revokedAt;
    }
}

} // namespace shortlink
