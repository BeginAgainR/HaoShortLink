#pragma once

#include <string>

namespace shortlink
{

class RateLimiter
{
public:
    enum class Status
    {
        Allowed,
        Limited,
        Error
    };

    struct Result
    {
        Status status;
        int retryAfterSeconds;
    };

    virtual ~RateLimiter() = default;

    virtual Result check(const std::string& key) const = 0;
};

} // namespace shortlink
