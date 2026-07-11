#pragma once

#include <string>

namespace http
{
namespace utils
{

inline std::string escapeJsonString(const std::string& value)
{
    std::string result;
    result.reserve(value.size());

    for (char c : value)
    {
        switch (c)
        {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += c;
            break;
        }
    }

    return result;
}

} // namespace utils
} // namespace http
