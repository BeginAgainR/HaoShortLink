#include "shortlink/AuthRepository.h"

namespace shortlink
{

const char* userStatusToString(AuthRepository::UserStatus status) noexcept
{
    return status == AuthRepository::UserStatus::Active ? "active" : "disabled";
}

} // namespace shortlink
