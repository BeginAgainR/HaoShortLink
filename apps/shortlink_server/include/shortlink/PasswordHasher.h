#pragma once

#include <optional>
#include <string>

namespace shortlink
{

class PasswordHasher
{
public:
    static std::optional<std::string> hash(const std::string& password);
    static bool verify(const std::string& password, const std::string& encodedHash);
};

} // namespace shortlink
