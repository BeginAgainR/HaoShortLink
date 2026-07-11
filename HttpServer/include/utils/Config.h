#pragma once

#include <string>
#include <unordered_map>

namespace http
{
namespace utils
{

class Config
{
public:
    bool loadFromFile(const std::string& path);

    bool has(const std::string& key) const;
    std::string getString(const std::string& key, const std::string& defaultValue) const;
    int getInt(const std::string& key, int defaultValue) const;
    bool getBool(const std::string& key, bool defaultValue) const;

private:
    static std::string trim(const std::string& value);
    static std::string toLower(std::string value);

private:
    std::unordered_map<std::string, std::string> values_;
};

} // namespace utils
} // namespace http
