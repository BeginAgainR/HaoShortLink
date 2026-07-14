#pragma once

#include "../http/HttpRequest.h"
#include "../http/HttpResponse.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>

namespace http
{
namespace metrics
{

class HttpMetrics
{
public:
    HttpMetrics() = default;

    void registerRoute(HttpRequest::Method method, const std::string& route);
    void record(HttpRequest::Method method,
                const std::string& route,
                HttpResponse::HttpStatusCode statusCode,
                double durationSeconds);
    std::string renderPrometheus() const;

private:
    static constexpr std::size_t kStatusClassCount = 5;
    static constexpr std::size_t kBucketCount = 13;

    struct RouteMetrics
    {
        RouteMetrics(std::string methodValue, std::string routeValue);

        std::string method;
        std::string route;
        std::array<std::atomic<std::uint64_t>, kStatusClassCount> statusCounts;
        std::array<std::atomic<std::uint64_t>, kBucketCount> bucketCounts;
        std::atomic<std::uint64_t> requestCount { 0 };
        std::atomic<std::uint64_t> durationNanoseconds { 0 };
    };

    static std::string methodToString(HttpRequest::Method method);
    static std::string makeKey(HttpRequest::Method method, const std::string& route);
    static std::size_t statusClassIndex(HttpResponse::HttpStatusCode statusCode);
    std::shared_ptr<RouteMetrics> findOrRegister(HttpRequest::Method method,
                                                 const std::string& route);

private:
    mutable std::shared_mutex mutex_;
    std::map<std::string, std::shared_ptr<RouteMetrics>> routes_;
};

} // namespace metrics
} // namespace http
