#include "shortlink/ShortLinkRepository.h"

namespace shortlink
{

const char* statusToString(ShortLinkRepository::Status status) noexcept
{
    return status == ShortLinkRepository::Status::Active ? "active" : "disabled";
}

std::optional<ShortLinkRepository::Status> parseStatus(const std::string& value) noexcept
{
    if (value == "active")
    {
        return ShortLinkRepository::Status::Active;
    }
    if (value == "disabled")
    {
        return ShortLinkRepository::Status::Disabled;
    }
    return std::nullopt;
}

} // namespace shortlink
