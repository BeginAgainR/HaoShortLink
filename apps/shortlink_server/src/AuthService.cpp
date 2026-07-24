#include "shortlink/AuthService.h"

#include "shortlink/PasswordHasher.h"

#include <algorithm>
#include <array>
#include <chrono>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace shortlink
{

namespace
{

std::string toHex(const unsigned char* data, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i)
    {
        result[i * 2] = digits[data[i] >> 4];
        result[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return result;
}

} // namespace

AuthService::AuthService(AuthRepository& repository, std::int64_t sessionTtlSeconds)
    : repository_(repository),
      sessionTtlSeconds_(sessionTtlSeconds),
      dummyPasswordHash_(PasswordHasher::hash("hao-shortlink-dummy-password").value_or("!"))
{}

AuthService::Result AuthService::registerUser(const std::string& username,
                                              const std::string& password)
{
    if (!isValidUsername(username))
    {
        return { ResultStatus::InvalidUsername, std::nullopt };
    }
    if (!isValidPassword(password))
    {
        return { ResultStatus::InvalidPassword, std::nullopt };
    }
    const std::optional<std::string> passwordHash = PasswordHasher::hash(password);
    if (!passwordHash)
    {
        return { ResultStatus::Error, std::nullopt };
    }
    const AuthRepository::CreateUserResult created = repository_.createUser(
        username,
        normalizeUsername(username),
        *passwordHash);
    if (created.status == AuthRepository::CreateUserStatus::Conflict || !created.user)
    {
        return { ResultStatus::UsernameConflict, std::nullopt };
    }
    const auto session = createSession(*created.user);
    return session ? Result { ResultStatus::Success, session }
                   : Result { ResultStatus::Error, std::nullopt };
}

AuthService::Result AuthService::login(const std::string& username,
                                       const std::string& password)
{
    const std::optional<AuthRepository::Credentials> credentials =
        repository_.findCredentials(normalizeUsername(username));
    const bool passwordMatches = PasswordHasher::verify(
        password,
        credentials ? credentials->passwordHash : dummyPasswordHash_);
    if (!credentials || credentials->user.status != AuthRepository::UserStatus::Active ||
        !passwordMatches)
    {
        return { ResultStatus::InvalidCredentials, std::nullopt };
    }
    const auto session = createSession(credentials->user);
    return session ? Result { ResultStatus::Success, session }
                   : Result { ResultStatus::Error, std::nullopt };
}

std::optional<AuthRepository::User> AuthService::authenticate(const std::string& token) const
{
    if (token.size() != 64)
    {
        return std::nullopt;
    }
    return repository_.findUserBySession(tokenHash(token), nowEpochSeconds());
}

void AuthService::logout(const std::string& token)
{
    if (token.size() == 64)
    {
        repository_.revokeSession(tokenHash(token), nowEpochSeconds());
    }
}

bool AuthService::isValidUsername(const std::string& username)
{
    const auto isAsciiLetter = [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
    };
    const auto isAsciiDigit = [](unsigned char ch) {
        return ch >= '0' && ch <= '9';
    };
    if (username.size() < 3 || username.size() > 32 ||
        !isAsciiLetter(static_cast<unsigned char>(username.front())))
    {
        return false;
    }
    return std::all_of(username.begin() + 1, username.end(), [&](unsigned char ch) {
        return isAsciiLetter(ch) || isAsciiDigit(ch) || ch == '_' || ch == '-';
    });
}

bool AuthService::isValidPassword(const std::string& password)
{
    return password.size() >= 10 && password.size() <= 128;
}

std::string AuthService::normalizeUsername(const std::string& username)
{
    std::string normalized = username;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a')
                                      : static_cast<char>(ch);
    });
    return normalized;
}

std::string AuthService::tokenHash(const std::string& token)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int digestSize = 0;
    if (EVP_Digest(token.data(),
                   token.size(),
                   digest.data(),
                   &digestSize,
                   EVP_sha256(),
                   nullptr) != 1)
    {
        return {};
    }
    return toHex(digest.data(), digestSize);
}

std::int64_t AuthService::nowEpochSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<AuthService::AuthenticatedSession> AuthService::createSession(
    const AuthRepository::User& user)
{
    const std::optional<std::string> token = generateToken();
    if (!token)
    {
        return std::nullopt;
    }
    const std::int64_t expiresAt = nowEpochSeconds() + sessionTtlSeconds_;
    const std::string digest = tokenHash(*token);
    if (digest.empty())
    {
        return std::nullopt;
    }
    repository_.createSession(user.id, digest, expiresAt);
    return AuthenticatedSession { user, *token, expiresAt };
}

std::optional<std::string> AuthService::generateToken()
{
    std::array<unsigned char, 32> token {};
    if (RAND_bytes(token.data(), static_cast<int>(token.size())) != 1)
    {
        return std::nullopt;
    }
    return toHex(token.data(), token.size());
}

} // namespace shortlink
