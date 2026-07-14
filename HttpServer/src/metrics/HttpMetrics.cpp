#include "../../include/metrics/HttpMetrics.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace http
{
namespace metrics
{
namespace
{

const std::array<double, 13> kDurationBuckets {
    0.0005,
    0.001,
    0.0025,
    0.005,
    0.01,
    0.025,
    0.05,
    0.1,
    0.25,
    0.5,
    1.0,
    2.5,
    5.0
};

const std::array<const char*, 13> kDurationBucketLabels {
    "0.0005",
    "0.001",
    "0.0025",
    "0.005",
    "0.01",
    "0.025",
    "0.05",
    "0.1",
    "0.25",
    "0.5",
    "1",
    "2.5",
    "5"
};

const std::array<const char*, 5> kStatusClassLabels {
    "2xx",
    "3xx",
    "4xx",
    "5xx",
    "other"
};

std::string escapeLabelValue(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value)
    {
        switch (ch)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

} // namespace

HttpMetrics::RouteMetrics::RouteMetrics(std::string methodValue, std::string routeValue)
    : method(std::move(methodValue)),
      route(std::move(routeValue))
{
    for (std::atomic<std::uint64_t>& count : statusCounts)
    {
        count.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<std::uint64_t>& count : bucketCounts)
    {
        count.store(0, std::memory_order_relaxed);
    }
}

void HttpMetrics::registerRoute(HttpRequest::Method method, const std::string& route)
{
    (void)findOrRegister(method, route);
}

void HttpMetrics::record(HttpRequest::Method method,
                         const std::string& route,
                         HttpResponse::HttpStatusCode statusCode,
                         double durationSeconds)
{
    const std::shared_ptr<RouteMetrics> routeMetrics = findOrRegister(method, route);
    routeMetrics->statusCounts[statusClassIndex(statusCode)].fetch_add(1, std::memory_order_relaxed);

    const double safeDuration = std::max(0.0, durationSeconds);
    const auto bucket = std::lower_bound(kDurationBuckets.begin(),
                                         kDurationBuckets.end(),
                                         safeDuration);
    if (bucket != kDurationBuckets.end())
    {
        const std::size_t index = static_cast<std::size_t>(bucket - kDurationBuckets.begin());
        routeMetrics->bucketCounts[index].fetch_add(1, std::memory_order_relaxed);
    }

    const double nanoseconds = safeDuration * 1000000000.0;
    const std::uint64_t roundedNanoseconds =
        nanoseconds >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(nanoseconds + 0.5);
    routeMetrics->durationNanoseconds.fetch_add(roundedNanoseconds, std::memory_order_relaxed);
    routeMetrics->requestCount.fetch_add(1, std::memory_order_relaxed);
}

std::string HttpMetrics::renderPrometheus() const
{
    std::vector<std::shared_ptr<RouteMetrics>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        snapshot.reserve(routes_.size());
        for (const auto& entry : routes_)
        {
            snapshot.push_back(entry.second);
        }
    }

    std::ostringstream output;
    output << "# HELP haohttp_http_requests_total Total completed HTTP requests.\n"
           << "# TYPE haohttp_http_requests_total counter\n";
    for (const std::shared_ptr<RouteMetrics>& routeMetrics : snapshot)
    {
        const std::string method = escapeLabelValue(routeMetrics->method);
        const std::string route = escapeLabelValue(routeMetrics->route);
        for (std::size_t i = 0; i < kStatusClassLabels.size(); ++i)
        {
            output << "haohttp_http_requests_total{method=\"" << method
                   << "\",route=\"" << route
                   << "\",status_class=\"" << kStatusClassLabels[i]
                   << "\"} " << routeMetrics->statusCounts[i].load(std::memory_order_relaxed)
                   << '\n';
        }
    }

    output << "# HELP haohttp_http_request_duration_seconds HTTP request duration in seconds.\n"
           << "# TYPE haohttp_http_request_duration_seconds histogram\n";
    for (const std::shared_ptr<RouteMetrics>& routeMetrics : snapshot)
    {
        const std::string method = escapeLabelValue(routeMetrics->method);
        const std::string route = escapeLabelValue(routeMetrics->route);
        std::uint64_t cumulativeCount = 0;
        for (std::size_t i = 0; i < kDurationBucketLabels.size(); ++i)
        {
            cumulativeCount += routeMetrics->bucketCounts[i].load(std::memory_order_relaxed);
            output << "haohttp_http_request_duration_seconds_bucket{method=\"" << method
                   << "\",route=\"" << route
                   << "\",le=\"" << kDurationBucketLabels[i]
                   << "\"} " << cumulativeCount << '\n';
        }

        const std::uint64_t requestCount = routeMetrics->requestCount.load(std::memory_order_relaxed);
        const std::uint64_t durationNanoseconds =
            routeMetrics->durationNanoseconds.load(std::memory_order_relaxed);
        output << "haohttp_http_request_duration_seconds_bucket{method=\"" << method
               << "\",route=\"" << route << "\",le=\"+Inf\"} " << requestCount << '\n'
               << "haohttp_http_request_duration_seconds_sum{method=\"" << method
               << "\",route=\"" << route << "\"} "
               << std::fixed << std::setprecision(9)
               << static_cast<double>(durationNanoseconds) / 1000000000.0 << '\n'
               << "haohttp_http_request_duration_seconds_count{method=\"" << method
               << "\",route=\"" << route << "\"} " << requestCount << '\n';
    }

    return output.str();
}

std::string HttpMetrics::methodToString(HttpRequest::Method method)
{
    switch (method)
    {
    case HttpRequest::kGet:
        return "GET";
    case HttpRequest::kPost:
        return "POST";
    case HttpRequest::kHead:
        return "HEAD";
    case HttpRequest::kPut:
        return "PUT";
    case HttpRequest::kDelete:
        return "DELETE";
    case HttpRequest::kOptions:
        return "OPTIONS";
    case HttpRequest::kInvalid:
    default:
        return "INVALID";
    }
}

std::string HttpMetrics::makeKey(HttpRequest::Method method, const std::string& route)
{
    return methodToString(method) + '\n' + route;
}

std::size_t HttpMetrics::statusClassIndex(HttpResponse::HttpStatusCode statusCode)
{
    const int status = static_cast<int>(statusCode);
    if (status >= 200 && status < 300)
    {
        return 0;
    }
    if (status >= 300 && status < 400)
    {
        return 1;
    }
    if (status >= 400 && status < 500)
    {
        return 2;
    }
    if (status >= 500 && status < 600)
    {
        return 3;
    }
    return 4;
}

std::shared_ptr<HttpMetrics::RouteMetrics>
HttpMetrics::findOrRegister(HttpRequest::Method method, const std::string& route)
{
    const std::string key = makeKey(method, route);
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const auto it = routes_.find(key);
        if (it != routes_.end())
        {
            return it->second;
        }
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto existing = routes_.find(key);
    if (existing != routes_.end())
    {
        return existing->second;
    }

    const std::shared_ptr<RouteMetrics> routeMetrics =
        std::make_shared<RouteMetrics>(methodToString(method), route);
    routes_[key] = routeMetrics;
    return routeMetrics;
}

} // namespace metrics
} // namespace http
