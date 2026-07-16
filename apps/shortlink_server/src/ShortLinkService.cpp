#include "shortlink/ShortLinkService.h"
#include "shortlink/ShortLinkMetrics.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace shortlink
{

namespace
{

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool isLeapYear(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(int year, int month)
{
    static const int days[] { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && isLeapYear(year))
    {
        return 29;
    }
    return days[month - 1];
}

std::int64_t daysFromCivil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = static_cast<unsigned>(
        static_cast<int>(month) + (month > 2 ? -3 : 9));
    const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<int>(dayOfEra) - 719468;
}

bool parseDigits(const std::string& value, std::size_t begin, std::size_t count, int* result)
{
    int parsed = 0;
    for (std::size_t i = begin; i < begin + count; ++i)
    {
        if (value[i] < '0' || value[i] > '9')
        {
            return false;
        }
        parsed = parsed * 10 + (value[i] - '0');
    }
    *result = parsed;
    return true;
}

ShortLinkMetrics::RedirectSource metricSource(ShortLinkRepository::LookupSource source)
{
    switch (source)
    {
    case ShortLinkRepository::LookupSource::Mysql:
        return ShortLinkMetrics::RedirectSource::Mysql;
    case ShortLinkRepository::LookupSource::Redis:
        return ShortLinkMetrics::RedirectSource::Redis;
    case ShortLinkRepository::LookupSource::Memory:
    default:
        return ShortLinkMetrics::RedirectSource::Memory;
    }
}

} // namespace

ShortLinkService::ShortLinkService(ShortLinkRepository& repository, ShortLinkMetrics* metrics)
    : repository_(repository),
      metrics_(metrics)
{}

std::optional<ShortLinkService::ShortLink> ShortLinkService::createShortLink(
    const std::string& originalUrl,
    std::optional<std::int64_t> expiresAt)
{
    if (!isValidUrl(originalUrl) || (expiresAt && *expiresAt <= nowEpochSeconds()))
    {
        return std::nullopt;
    }

    const std::optional<ShortLinkRepository::ShortLinkRecord> record =
        repository_.create(originalUrl, expiresAt);
    if (!record)
    {
        return std::nullopt;
    }

    return ShortLink {
        *record,
        "/s/" + record->code
    };
}

ShortLinkService::RedirectResult ShortLinkService::resolve(const std::string& code) const
{
    ShortLinkRepository::LookupResult lookup;
    try
    {
        lookup = repository_.findByCode(code);
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Error,
                                     metricSource(repository_.defaultLookupSource()));
        }
        throw;
    }
    const ShortLinkMetrics::RedirectSource source = metricSource(lookup.source);
    if (!lookup.record)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::NotFound, source);
        }
        return { RedirectStatus::NotFound, std::nullopt };
    }
    if (lookup.record->status == ShortLinkRepository::Status::Disabled)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Disabled, source);
        }
        return { RedirectStatus::Disabled, std::nullopt };
    }
    if (isExpired(*lookup.record, nowEpochSeconds()))
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Expired, source);
        }
        return { RedirectStatus::Expired, std::nullopt };
    }
    if (metrics_ != nullptr)
    {
        metrics_->recordRedirect(ShortLinkMetrics::RedirectResult::Success, source);
    }
    return { RedirectStatus::Success, lookup.record->originalUrl };
}

std::optional<ShortLinkRepository::ShortLinkRecord> ShortLinkService::get(
    const std::string& code) const
{
    return repository_.findByCode(code).record;
}

std::vector<ShortLinkRepository::ShortLinkRecord> ShortLinkService::list(
    const ShortLinkRepository::ListQuery& query) const
{
    return repository_.list(query);
}

std::optional<ShortLinkRepository::ShortLinkRecord> ShortLinkService::updateLifecycle(
    const std::string& code,
    const ShortLinkRepository::LifecycleUpdate& update)
{
    return repository_.updateLifecycle(code, update);
}

bool ShortLinkService::isValidUrl(const std::string& url) const
{
    return startsWith(url, "http://") || startsWith(url, "https://");
}

bool ShortLinkService::isExpired(const ShortLinkRepository::ShortLinkRecord& record,
                                 std::int64_t nowEpochSeconds)
{
    return record.expiresAt && *record.expiresAt <= nowEpochSeconds;
}

std::optional<std::int64_t> ShortLinkService::parseUtcTimestamp(const std::string& value)
{
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z')
    {
        return std::nullopt;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parseDigits(value, 0, 4, &year) || !parseDigits(value, 5, 2, &month) ||
        !parseDigits(value, 8, 2, &day) || !parseDigits(value, 11, 2, &hour) ||
        !parseDigits(value, 14, 2, &minute) || !parseDigits(value, 17, 2, &second) ||
        year < 1970 || month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month) ||
        hour > 23 || minute > 59 || second > 59)
    {
        return std::nullopt;
    }

    return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
           hour * 3600 + minute * 60 + second;
}

std::string ShortLinkService::formatUtcTimestamp(std::int64_t epochSeconds)
{
    const std::time_t value = static_cast<std::time_t>(epochSeconds);
    std::tm utc {};
    gmtime_r(&value, &utc);
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
    {
        return {};
    }
    return buffer;
}

std::int64_t ShortLinkService::nowEpochSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace shortlink
