#include "../../include/utils/RequestId.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <random>

namespace http
{
namespace utils
{
namespace
{

uint64_t createProcessNonce()
{
    try
    {
        std::random_device random;
        return (static_cast<uint64_t>(random()) << 32) ^ static_cast<uint64_t>(random());
    }
    catch (...)
    {
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
}

} // namespace

bool isValidRequestId(const std::string& requestId)
{
    if (requestId.empty() || requestId.size() > 64)
    {
        return false;
    }

    for (const unsigned char ch : requestId)
    {
        const bool valid = (ch >= 'a' && ch <= 'z') ||
                           (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9') ||
                           ch == '.' || ch == '_' || ch == '-';
        if (!valid)
        {
            return false;
        }
    }

    return true;
}

std::string generateRequestId()
{
    static const uint64_t processNonce = createProcessNonce();
    static std::atomic<uint64_t> sequence { 0 };

    const uint64_t next = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    char value[33];
    std::snprintf(value,
                  sizeof(value),
                  "%016llx%016llx",
                  static_cast<unsigned long long>(processNonce),
                  static_cast<unsigned long long>(next));
    return std::string(value);
}

} // namespace utils
} // namespace http
