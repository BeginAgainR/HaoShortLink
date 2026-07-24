#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace shortlink
{

class AuthRepository
{
public:
    enum class UserStatus
    {
        Active,
        Disabled
    };

    struct User
    {
        std::uint64_t id { 0 };
        std::string username;
        UserStatus status { UserStatus::Active };
        std::int64_t createdAt { 0 };
        std::int64_t updatedAt { 0 };
    };

    struct Credentials
    {
        User user;
        std::string passwordHash;
    };

    enum class CreateUserStatus
    {
        Created,
        Conflict
    };

    struct CreateUserResult
    {
        CreateUserStatus status { CreateUserStatus::Conflict };
        std::optional<User> user;
    };

    virtual ~AuthRepository() = default;

    virtual CreateUserResult createUser(const std::string& username,
                                        const std::string& normalizedUsername,
                                        const std::string& passwordHash) = 0;
    virtual std::optional<Credentials> findCredentials(
        const std::string& normalizedUsername) const = 0;
    virtual void createSession(std::uint64_t userId,
                               const std::string& tokenHash,
                               std::int64_t expiresAt) = 0;
    virtual std::optional<User> findUserBySession(const std::string& tokenHash,
                                                  std::int64_t nowEpochSeconds) const = 0;
    virtual void revokeSession(const std::string& tokenHash,
                               std::int64_t revokedAt) = 0;
};

const char* userStatusToString(AuthRepository::UserStatus status) noexcept;

} // namespace shortlink
