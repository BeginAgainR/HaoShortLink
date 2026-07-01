#include "../../include/utils/Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace http
{
namespace utils
{

bool Config::loadFromFile(const std::string& path)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        return false;
    }

    std::unordered_map<std::string, std::string> loadedValues;

    std::string line;
    while (std::getline(input, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const std::string::size_type separator = line.find('=');
        if (separator == std::string::npos)
        {
            return false;
        }

        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        if (key.empty())
        {
            return false;
        }

        loadedValues[key] = value;
    }

    values_ = std::move(loadedValues);
    return true;
}

bool Config::has(const std::string& key) const
{
    return values_.find(key) != values_.end();
}

std::string Config::getString(const std::string& key, const std::string& defaultValue) const
{
    const auto it = values_.find(key);
    if (it == values_.end())
    {
        return defaultValue;
    }
    return it->second;
}

int Config::getInt(const std::string& key, int defaultValue) const
{
    const auto it = values_.find(key);
    if (it == values_.end())
    {
        return defaultValue;
    }

    try
    {
        std::size_t parsedChars = 0;
        const int value = std::stoi(it->second, &parsedChars);
        if (parsedChars != it->second.size())
        {
            return defaultValue;
        }
        return value;
    }
    catch (const std::exception&)
    {
        return defaultValue;
    }
}

bool Config::getBool(const std::string& key, bool defaultValue) const
{
    const auto it = values_.find(key);
    if (it == values_.end())
    {
        return defaultValue;
    }

    const std::string value = toLower(trim(it->second));
    if (value == "true" || value == "1" || value == "yes" || value == "on")
    {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off")
    {
        return false;
    }
    return defaultValue;
}

std::string Config::trim(const std::string& value)
{
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    if (begin == value.end())
    {
        return "";
    }

    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();

    return std::string(begin, end);
}

std::string Config::toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace utils
} // namespace http
