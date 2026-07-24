#pragma once

#include "shortlink/AuthRepository.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace shortlink
{

class MemoryAuthRepository : public AuthRepository
{
public:
    CreateUserResult createUser(const std::string& username,
                                const std::string& normalizedUsername,
                                const std::string& passwordHash) override;
    std::optional<Credentials> findCredentials(
        const std::string& normalizedUsername) const override;
    void createSession(std::uint64_t userId,
                       const std::string& tokenHash,
                       std::int64_t expiresAt) override;
    std::optional<User> findUserBySession(const std::string& tokenHash,
                                          std::int64_t nowEpochSeconds) const override;
    void revokeSession(const std::string& tokenHash,
                       std::int64_t revokedAt) override;

private:
    struct StoredUser
    {
        User user;
        std::string normalizedUsername;
        std::string passwordHash;
    };

    struct StoredSession
    {
        std::uint64_t userId { 0 };
        std::int64_t expiresAt { 0 };
        std::optional<std::int64_t> revokedAt;
    };

    mutable std::mutex mutex_;
    std::uint64_t nextUserId_ { 1 };
    std::unordered_map<std::uint64_t, StoredUser> users_;
    std::unordered_map<std::string, std::uint64_t> userIdsByName_;
    std::unordered_map<std::string, StoredSession> sessions_;
};

} // namespace shortlink
