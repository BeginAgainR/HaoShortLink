#pragma once

#include "shortlink/AuthRepository.h"

#include <cstdint>
#include <optional>
#include <string>

namespace shortlink
{

class AuthService
{
public:
    enum class ResultStatus
    {
        Success,
        InvalidUsername,
        InvalidPassword,
        UsernameConflict,
        InvalidCredentials,
        Error
    };

    struct AuthenticatedSession
    {
        AuthRepository::User user;
        std::string token;
        std::int64_t expiresAt { 0 };
    };

    struct Result
    {
        ResultStatus status { ResultStatus::Error };
        std::optional<AuthenticatedSession> session;
    };

    explicit AuthService(AuthRepository& repository,
                         std::int64_t sessionTtlSeconds = 7 * 24 * 60 * 60);

    Result registerUser(const std::string& username, const std::string& password);
    Result login(const std::string& username, const std::string& password);
    std::optional<AuthRepository::User> authenticate(const std::string& token) const;
    void logout(const std::string& token);

    static bool isValidUsername(const std::string& username);
    static bool isValidPassword(const std::string& password);
    static std::string normalizeUsername(const std::string& username);
    static std::string tokenHash(const std::string& token);
    static std::int64_t nowEpochSeconds();

private:
    std::optional<AuthenticatedSession> createSession(const AuthRepository::User& user);
    static std::optional<std::string> generateToken();

    AuthRepository& repository_;
    std::int64_t sessionTtlSeconds_;
    std::string dummyPasswordHash_;
};

} // namespace shortlink
