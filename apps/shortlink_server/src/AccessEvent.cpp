#include "shortlink/AccessEvent.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace shortlink
{
namespace
{

constexpr std::size_t kEventIdLength = 32;
constexpr std::size_t kMaxRequestIdLength = 64;
constexpr std::size_t kMaxCodeLength = 128;

std::uint64_t createProcessNonce()
{
    try
    {
        std::random_device random;
        return (static_cast<std::uint64_t>(random()) << 32) ^
               static_cast<std::uint64_t>(random());
    }
    catch (...)
    {
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
}

bool isLowercaseHex(const std::string& value)
{
    if (value.size() != kEventIdLength)
    {
        return false;
    }

    for (const unsigned char ch : value)
    {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
        {
            return false;
        }
    }
    return true;
}

bool isSafeRequestId(const std::string& value)
{
    if (value.empty() || value.size() > kMaxRequestIdLength)
    {
        return false;
    }

    for (const unsigned char ch : value)
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

int expectedHttpStatus(AccessEventResult result)
{
    switch (result)
    {
    case AccessEventResult::Success:
        return 302;
    case AccessEventResult::NotFound:
    case AccessEventResult::Disabled:
    case AccessEventResult::Expired:
        return 404;
    case AccessEventResult::Error:
        return 500;
    }
    return 0;
}

} // namespace

std::string accessEventResultToString(AccessEventResult result)
{
    switch (result)
    {
    case AccessEventResult::Success:
        return "success";
    case AccessEventResult::NotFound:
        return "not_found";
    case AccessEventResult::Disabled:
        return "disabled";
    case AccessEventResult::Expired:
        return "expired";
    case AccessEventResult::Error:
        return "error";
    }
    return "error";
}

std::optional<AccessEventResult> parseAccessEventResult(const std::string& value)
{
    if (value == "success")
    {
        return AccessEventResult::Success;
    }
    if (value == "not_found")
    {
        return AccessEventResult::NotFound;
    }
    if (value == "disabled")
    {
        return AccessEventResult::Disabled;
    }
    if (value == "expired")
    {
        return AccessEventResult::Expired;
    }
    if (value == "error")
    {
        return AccessEventResult::Error;
    }
    return std::nullopt;
}

bool isValidAccessEvent(const AccessEvent& event) noexcept
{
    return isLowercaseHex(event.eventId) &&
           event.occurredAtMs > 0 &&
           isSafeRequestId(event.requestId) &&
           !event.code.empty() &&
           event.code.size() <= kMaxCodeLength &&
           event.httpStatus == expectedHttpStatus(event.result);
}

std::string serializeAccessEvent(const AccessEvent& event)
{
    if (!isValidAccessEvent(event))
    {
        throw std::invalid_argument("invalid access event");
    }

    nlohmann::ordered_json payload;
    payload["schema_version"] = kAccessEventSchemaVersion;
    payload["event_type"] = kAccessEventType;
    payload["event_id"] = event.eventId;
    payload["occurred_at_ms"] = event.occurredAtMs;
    payload["request_id"] = event.requestId;
    payload["code"] = event.code;
    payload["result"] = accessEventResultToString(event.result);
    payload["http_status"] = event.httpStatus;
    return payload.dump();
}

std::optional<AccessEvent> parseAccessEvent(const std::string& payload) noexcept
{
    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(payload);
        if (!parsed.is_object() ||
            !parsed.contains("schema_version") ||
            !parsed["schema_version"].is_number_integer() ||
            parsed["schema_version"].get<int>() != kAccessEventSchemaVersion ||
            !parsed.contains("event_type") ||
            !parsed["event_type"].is_string() ||
            parsed["event_type"].get<std::string>() != kAccessEventType ||
            !parsed.contains("event_id") || !parsed["event_id"].is_string() ||
            !parsed.contains("occurred_at_ms") || !parsed["occurred_at_ms"].is_number_integer() ||
            !parsed.contains("request_id") || !parsed["request_id"].is_string() ||
            !parsed.contains("code") || !parsed["code"].is_string() ||
            !parsed.contains("result") || !parsed["result"].is_string() ||
            !parsed.contains("http_status") || !parsed["http_status"].is_number_integer())
        {
            return std::nullopt;
        }

        const std::optional<AccessEventResult> result =
            parseAccessEventResult(parsed["result"].get<std::string>());
        if (!result)
        {
            return std::nullopt;
        }

        AccessEvent event;
        event.eventId = parsed["event_id"].get<std::string>();
        event.occurredAtMs = parsed["occurred_at_ms"].get<std::int64_t>();
        event.requestId = parsed["request_id"].get<std::string>();
        event.code = parsed["code"].get<std::string>();
        event.result = *result;
        event.httpStatus = parsed["http_status"].get<int>();
        if (!isValidAccessEvent(event))
        {
            return std::nullopt;
        }
        return event;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::string generateAccessEventId()
{
    static const std::uint64_t processNonce = createProcessNonce();
    static std::atomic<std::uint64_t> sequence { 0 };

    const std::uint64_t next = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    char value[kEventIdLength + 1];
    std::snprintf(value,
                  sizeof(value),
                  "%016llx%016llx",
                  static_cast<unsigned long long>(processNonce),
                  static_cast<unsigned long long>(next));
    return std::string(value);
}

std::int64_t nowEpochMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace shortlink
