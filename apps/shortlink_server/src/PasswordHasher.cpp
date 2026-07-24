#include "shortlink/PasswordHasher.h"

#include <array>
#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace shortlink
{

namespace
{

constexpr std::uint64_t kScryptN = 32768;
constexpr std::uint64_t kScryptR = 8;
constexpr std::uint64_t kScryptP = 1;
constexpr std::uint64_t kScryptMaxMemory = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

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

std::optional<std::vector<unsigned char>> fromHex(const std::string& value)
{
    if (value.size() % 2 != 0)
    {
        return std::nullopt;
    }
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::vector<unsigned char> bytes(value.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        const int high = digit(value[i * 2]);
        const int low = digit(value[i * 2 + 1]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return bytes;
}

std::vector<std::string> split(const std::string& value, char separator)
{
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true)
    {
        const std::size_t end = value.find(separator, begin);
        parts.push_back(value.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos)
        {
            return parts;
        }
        begin = end + 1;
    }
}

bool derive(const std::string& password,
            const std::vector<unsigned char>& salt,
            std::uint64_t n,
            std::uint64_t r,
            std::uint64_t p,
            unsigned char* output,
            std::size_t outputSize)
{
    return EVP_PBE_scrypt(password.data(),
                          password.size(),
                          salt.data(),
                          salt.size(),
                          n,
                          r,
                          p,
                          kScryptMaxMemory,
                          output,
                          outputSize) == 1;
}

} // namespace

std::optional<std::string> PasswordHasher::hash(const std::string& password)
{
    std::array<unsigned char, kSaltBytes> salt {};
    std::array<unsigned char, kHashBytes> derived {};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        !derive(password,
                std::vector<unsigned char>(salt.begin(), salt.end()),
                kScryptN,
                kScryptR,
                kScryptP,
                derived.data(),
                derived.size()))
    {
        return std::nullopt;
    }
    return "scrypt$" + std::to_string(kScryptN) + "$" + std::to_string(kScryptR) + "$" +
           std::to_string(kScryptP) + "$" + toHex(salt.data(), salt.size()) + "$" +
           toHex(derived.data(), derived.size());
}

bool PasswordHasher::verify(const std::string& password, const std::string& encodedHash)
{
    const std::vector<std::string> parts = split(encodedHash, '$');
    if (parts.size() != 6 || parts[0] != "scrypt")
    {
        return false;
    }

    std::uint64_t n = 0;
    std::uint64_t r = 0;
    std::uint64_t p = 0;
    try
    {
        n = std::stoull(parts[1]);
        r = std::stoull(parts[2]);
        p = std::stoull(parts[3]);
    }
    catch (...)
    {
        return false;
    }
    if (n != kScryptN || r != kScryptR || p != kScryptP)
    {
        return false;
    }
    const auto salt = fromHex(parts[4]);
    const auto expected = fromHex(parts[5]);
    if (!salt || salt->size() != kSaltBytes || !expected || expected->size() != kHashBytes)
    {
        return false;
    }
    std::array<unsigned char, kHashBytes> actual {};
    if (!derive(password, *salt, n, r, p, actual.data(), actual.size()))
    {
        return false;
    }
    return CRYPTO_memcmp(actual.data(), expected->data(), expected->size()) == 0;
}

} // namespace shortlink
