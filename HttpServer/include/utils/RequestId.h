#pragma once

#include <string>

namespace http
{
namespace utils
{

bool isValidRequestId(const std::string& requestId);
std::string generateRequestId();

} // namespace utils
} // namespace http
