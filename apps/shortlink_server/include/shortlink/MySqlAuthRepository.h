#pragma once

#include "shortlink/AuthRepository.h"

namespace sql
{
class ResultSet;
}

namespace shortlink
{

class MySqlAuthRepository : public AuthRepository
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
    static User readUser(sql::ResultSet* result);
};

} // namespace shortlink
