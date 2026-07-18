#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace shortlink
{

constexpr int kAccessEventSchemaVersion = 1;
constexpr const char* kAccessEventType = "short_link_access";

enum class AccessEventResult
{
    Success,
    NotFound,
    Disabled,
    Expired,
    Error
};

struct AccessEvent
{
    std::string eventId;
    std::int64_t occurredAtMs { 0 };
    std::string requestId;
    std::string code;
    AccessEventResult result { AccessEventResult::Error };
    int httpStatus { 500 };
};

std::string accessEventResultToString(AccessEventResult result);
std::optional<AccessEventResult> parseAccessEventResult(const std::string& value);
bool isValidAccessEvent(const AccessEvent& event) noexcept;
std::string serializeAccessEvent(const AccessEvent& event);
std::optional<AccessEvent> parseAccessEvent(const std::string& payload) noexcept;
std::string generateAccessEventId();
std::int64_t nowEpochMilliseconds();

} // namespace shortlink
