#pragma once

#include "shortlink/AccessEvent.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace http
{
namespace db
{
class DbConnection;
}
}

namespace shortlink
{
namespace consumer
{

struct AccessEventSourcePosition
{
    std::string topic;
    std::int32_t partition { -1 };
    std::int64_t offset { -1 };
};

enum class AccessStatisticsWriteResult
{
    Aggregated,
    Duplicate,
    IgnoredNotFound
};

class OrphanShortLinkEvent : public std::runtime_error
{
public:
    explicit OrphanShortLinkEvent(const std::string& code)
        : std::runtime_error("access event does not reference an existing short link: " + code)
    {}
};

class MySqlAccessStatisticsWriter
{
public:
    struct Config
    {
        std::string host = "tcp://127.0.0.1:3306";
        std::string user = "root";
        std::string password;
        std::string database = "hao_shortlink";
    };

    explicit MySqlAccessStatisticsWriter(Config config);
    ~MySqlAccessStatisticsWriter();

    MySqlAccessStatisticsWriter(const MySqlAccessStatisticsWriter&) = delete;
    MySqlAccessStatisticsWriter& operator=(const MySqlAccessStatisticsWriter&) = delete;

    AccessStatisticsWriteResult process(const AccessEvent& event,
                                        const AccessEventSourcePosition& source);
    void reconnect();

private:
    Config config_;
    std::unique_ptr<http::db::DbConnection> connection_;
};

} // namespace consumer
} // namespace shortlink
