#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "middleware/MiddlewareChain.h"
#include "metrics/HttpMetrics.h"
#include "router/Router.h"
#include "shortlink/AccessEvent.h"
#include "shortlink/AccessEventPublisher.h"
#include "shortlink/AccessStatistics.h"
#include "shortlink/AuthService.h"
#include "shortlink/ConsumerMetrics.h"
#include "shortlink/KafkaDeadLetterPublisher.h"
#include "shortlink/MemoryShortLinkRepository.h"
#include "shortlink/MemoryAuthRepository.h"
#include "shortlink/PasswordHasher.h"
#include "shortlink/RedirectHandler.h"
#include "shortlink/SameOriginPolicy.h"
#include "shortlink/ShortLinkService.h"
#include "shortlink/ShortLinkMetrics.h"
#include "utils/RequestId.h"

#include <muduo/net/Buffer.h>

#include <iostream>
#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

class TestFailure : public std::runtime_error
{
public:
    explicit TestFailure(const std::string& message)
        : std::runtime_error(message)
    {}
};

using TestFunc = void (*)();

struct TestCase
{
    std::string name;
    TestFunc run;
};

void expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw TestFailure(message);
    }
}

http::HttpRequest makeRequest(http::HttpRequest::Method method, const std::string& path)
{
    http::HttpRequest req;
    switch (method)
    {
    case http::HttpRequest::kGet:
        req.setMethod("GET", "GET" + 3);
        break;
    case http::HttpRequest::kPost:
        req.setMethod("POST", "POST" + 4);
        break;
    default:
        throw TestFailure("Unsupported test request method");
    }

    req.setPath(path.data(), path.data() + path.size());
    return req;
}

void addHeader(http::HttpRequest* request,
               const std::string& name,
               const std::string& value)
{
    const std::string header = name + ": " + value;
    const char* start = header.data();
    request->addHeader(start, start + name.size(), start + header.size());
}

std::string responseToString(const http::HttpResponse& response)
{
    muduo::net::Buffer buffer;
    response.appendToBuffer(&buffer);
    return buffer.retrieveAllAsString();
}

void testRouterExactCallback()
{
    http::router::Router router;
    http::HttpRequest req = makeRequest(http::HttpRequest::kGet, "/api/health");
    http::HttpResponse resp(false);
    bool called = false;
    std::string matchedRoute;

    router.registerCallback(http::HttpRequest::kGet,
                            "/api/health",
                            [&called](const http::HttpRequest& routedReq, http::HttpResponse* routedResp) {
                                called = true;
                                expect(routedReq.path() == "/api/health", "exact callback should receive original path");
                                routedResp->setStatusCode(http::HttpResponse::k200Ok);
                                routedResp->setStatusMessage("OK");
                            });

    expect(router.route(req, &resp, &matchedRoute), "exact route should match");
    expect(called, "exact route callback should be called");
    expect(matchedRoute == "/api/health", "exact route should return its registered path");
    expect(resp.getStatusCode() == http::HttpResponse::k200Ok, "exact route should update response");
}

void testRouterDynamicCallbackPathParameters()
{
    http::router::Router router;
    http::HttpRequest req = makeRequest(http::HttpRequest::kGet, "/s/abc123");
    http::HttpResponse resp(false);
    bool called = false;
    std::string matchedRoute;

    router.addRegexCallback(http::HttpRequest::kGet,
                            "/s/:code",
                            [&called](const http::HttpRequest& routedReq, http::HttpResponse* routedResp) {
                                called = true;
                                expect(routedReq.getPathParameters("param1") == "abc123",
                                       "dynamic route should expose first path parameter");
                                routedResp->setStatusCode(http::HttpResponse::k302Found);
                                routedResp->setStatusMessage("Found");
                            });

    expect(router.route(req, &resp, &matchedRoute), "dynamic route should match");
    expect(called, "dynamic route callback should be called");
    expect(matchedRoute == "/s/:code", "dynamic route should return its registered pattern");
    expect(resp.getStatusCode() == http::HttpResponse::k302Found, "dynamic route should update response");
}

void testRouterNoMatch()
{
    http::router::Router router;
    http::HttpRequest req = makeRequest(http::HttpRequest::kGet, "/missing");
    http::HttpResponse resp(false);

    expect(!router.route(req, &resp), "unknown route should not match");
    expect(resp.getStatusCode() == http::HttpResponse::kUnknown, "unknown route should not mutate response status");
}

void testRequestSwapClearsBodyState()
{
    http::HttpRequest req = makeRequest(http::HttpRequest::kPost, "/api/short-links");
    req.setBody("{\"url\":\"https://example.com\"}");
    req.setContentLength(req.getBody().size());
    req.setPathParameters("param1", "old");
    req.setRequestId("old-request-id");

    http::HttpRequest empty;
    req.swap(empty);

    expect(req.getBody().empty(), "swap with default request should clear body");
    expect(req.contentLength() == 0, "swap with default request should clear content length");
    expect(req.getPathParameters("param1").empty(), "swap with default request should clear path parameters");
    expect(req.requestId().empty(), "swap with default request should clear request ID");
}

void testRequestHeaderLookupIsCaseInsensitive()
{
    http::HttpRequest req;
    const std::string header = "x-request-id: client-id";
    const char* start = header.data();
    const char* colon = start + header.find(':');
    req.addHeader(start, colon, start + header.size());

    expect(req.getHeader("X-Request-ID") == "client-id",
           "HTTP header lookup should be case insensitive");
}

void testSameOriginPolicy()
{
    http::HttpRequest sameOrigin = makeRequest(http::HttpRequest::kPost, "/api/auth/login");
    addHeader(&sameOrigin, "Host", "links.example.test");
    addHeader(&sameOrigin, "X-Forwarded-Proto", "https");
    addHeader(&sameOrigin, "Origin", "https://links.example.test");
    expect(shortlink::isSameOriginRequest(sameOrigin),
           "matching forwarded scheme and host should pass same-origin policy");

    http::HttpRequest crossOrigin = makeRequest(http::HttpRequest::kPost, "/api/short-links");
    addHeader(&crossOrigin, "Host", "links.example.test");
    addHeader(&crossOrigin, "X-Forwarded-Proto", "https");
    addHeader(&crossOrigin, "Origin", "https://attacker.example.test");
    expect(!shortlink::isSameOriginRequest(crossOrigin),
           "a different Origin authority should be rejected");

    http::HttpRequest fetchMetadata = makeRequest(http::HttpRequest::kPost, "/api/short-links");
    addHeader(&fetchMetadata, "Sec-Fetch-Site", "cross-site");
    expect(!shortlink::isSameOriginRequest(fetchMetadata),
           "cross-site Fetch Metadata should be rejected even without Origin");

    http::HttpRequest nonBrowser = makeRequest(http::HttpRequest::kPost, "/api/short-links");
    expect(shortlink::isSameOriginRequest(nonBrowser),
           "non-browser API clients may omit browser origin headers");
}

void testRequestIdValidationAndGeneration()
{
    expect(http::utils::isValidRequestId("client.Request_ID-01"),
           "request ID should accept the documented safe character set");
    expect(!http::utils::isValidRequestId(""), "empty request ID should be rejected");
    expect(!http::utils::isValidRequestId("request id"), "request ID with spaces should be rejected");
    expect(!http::utils::isValidRequestId(std::string(65, 'a')), "request ID over 64 characters should be rejected");

    const std::string first = http::utils::generateRequestId();
    const std::string second = http::utils::generateRequestId();
    expect(first.size() == 32, "generated request ID should contain 32 characters");
    expect(first != second, "generated request IDs should be unique within the process");
    for (const unsigned char ch : first)
    {
        expect(std::isdigit(ch) || (ch >= 'a' && ch <= 'f'),
               "generated request ID should use lowercase hexadecimal characters");
    }
}

void testRequestIdConcurrentGeneration()
{
    constexpr int threadCount = 8;
    constexpr int idsPerThread = 1000;
    std::unordered_set<std::string> ids;
    std::mutex idsMutex;
    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i)
    {
        threads.emplace_back([&ids, &idsMutex]() {
            for (int j = 0; j < idsPerThread; ++j)
            {
                const std::string requestId = http::utils::generateRequestId();
                std::lock_guard<std::mutex> lock(idsMutex);
                ids.insert(requestId);
            }
        });
    }

    for (std::thread& thread : threads)
    {
        thread.join();
    }

    expect(ids.size() == static_cast<size_t>(threadCount * idsPerThread),
           "concurrent request ID generation should not produce duplicates");
}

void testAccessEventRoundTrip()
{
    const std::vector<std::pair<shortlink::AccessEventResult, int>> cases {
        { shortlink::AccessEventResult::Success, 302 },
        { shortlink::AccessEventResult::NotFound, 404 },
        { shortlink::AccessEventResult::Disabled, 404 },
        { shortlink::AccessEventResult::Expired, 404 },
        { shortlink::AccessEventResult::Error, 500 }
    };

    for (const auto& item : cases)
    {
        const shortlink::AccessEvent event {
            shortlink::generateAccessEventId(),
            shortlink::nowEpochMilliseconds(),
            "access-event-test-request",
            "code-\"escaped",
            item.first,
            item.second
        };
        const std::string payload = shortlink::serializeAccessEvent(event);
        const std::optional<shortlink::AccessEvent> parsed = shortlink::parseAccessEvent(payload);

        expect(parsed.has_value(), "serialized access event should parse");
        expect(parsed->eventId == event.eventId, "access event ID should round trip");
        expect(parsed->occurredAtMs == event.occurredAtMs,
               "access event timestamp should round trip");
        expect(parsed->requestId == event.requestId, "access event request ID should round trip");
        expect(event.eventId != event.requestId,
               "access event ID should be independent from request ID");
        expect(parsed->code == event.code, "access event code should round trip with escaping");
        expect(parsed->result == event.result, "access event result should round trip");
        expect(parsed->httpStatus == event.httpStatus, "access event HTTP status should round trip");
        expect(payload.find("original_url") == std::string::npos,
               "access event should not contain original URL");
        expect(payload.find("user_agent") == std::string::npos,
               "access event should not contain User-Agent");
    }
}

void testAccessEventRejectsInvalidPayloads()
{
    const std::string validId = shortlink::generateAccessEventId();
    const std::string base =
        "{\"schema_version\":1,\"event_type\":\"short_link_access\",\"event_id\":\"" +
        validId +
        "\",\"occurred_at_ms\":1784304000123,\"request_id\":\"request-01\"," +
        "\"code\":\"000001\",\"result\":\"success\",\"http_status\":302}";
    expect(shortlink::parseAccessEvent(base).has_value(), "valid access event fixture should parse");

    std::string forwardCompatible = base;
    forwardCompatible.insert(forwardCompatible.size() - 1, ",\"future_field\":\"ignored\"");
    expect(shortlink::parseAccessEvent(forwardCompatible).has_value(),
           "unknown optional fields should remain forward compatible");

    expect(!shortlink::parseAccessEvent("not-json").has_value(),
           "invalid JSON access event should be rejected");
    expect(shortlink::parseAccessEventDetailed("not-json").error ==
               shortlink::AccessEventParseError::InvalidJson,
           "invalid JSON should retain a stable parse reason");
    expect(shortlink::parseAccessEventDetailed(
               "{\"schema_version\":1,\"event_type\":\"short_link_access\","
               "\"event_id\":\"0123456789abcdef0123456789abcdef\","
               "\"occurred_at_ms\":18446744073709551615,\"request_id\":\"request-1\","
               "\"code\":\"000001\",\"result\":\"success\",\"http_status\":302}")
               .error == shortlink::AccessEventParseError::InvalidContract,
           "values outside the event contract should not be classified as invalid JSON");
    expect(!shortlink::parseAccessEvent(
                "{\"schema_version\":2,\"event_type\":\"short_link_access\"}")
                .has_value(),
           "unsupported access event schema should be rejected");
    expect(shortlink::parseAccessEventDetailed(
               "{\"schema_version\":2,\"event_type\":\"short_link_access\"}")
               .error == shortlink::AccessEventParseError::UnsupportedSchema,
           "unsupported schema should retain a stable parse reason");

    expect(shortlink::parseAccessEventDetailed(
               "{\"schema_version\":1,\"event_type\":\"unknown\"}")
               .error == shortlink::AccessEventParseError::UnsupportedEventType,
           "unsupported event type should retain a stable parse reason");

    std::string missingRequestId = base;
    const std::string requestField = "\"request_id\":\"request-01\",";
    missingRequestId.erase(missingRequestId.find(requestField), requestField.size());
    expect(!shortlink::parseAccessEvent(missingRequestId).has_value(),
           "missing required access event fields should be rejected");

    std::string wrongResult = base;
    wrongResult.replace(wrongResult.find("success"), 7, "unknown");
    expect(!shortlink::parseAccessEvent(wrongResult).has_value(),
           "unknown access event result should be rejected");

    std::string wrongStatus = base;
    wrongStatus.replace(wrongStatus.rfind("302"), 3, "404");
    expect(!shortlink::parseAccessEvent(wrongStatus).has_value(),
           "result and HTTP status mismatch should be rejected");
}

void testAccessEventIdGeneration()
{
    constexpr int threadCount = 8;
    constexpr int idsPerThread = 1000;
    std::unordered_set<std::string> ids;
    std::mutex idsMutex;
    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i)
    {
        threads.emplace_back([&ids, &idsMutex]() {
            for (int j = 0; j < idsPerThread; ++j)
            {
                const std::string eventId = shortlink::generateAccessEventId();
                std::lock_guard<std::mutex> lock(idsMutex);
                ids.insert(eventId);
            }
        });
    }

    for (std::thread& thread : threads)
    {
        thread.join();
    }
    expect(ids.size() == static_cast<std::size_t>(threadCount * idsPerThread),
           "concurrent event ID generation should not produce duplicates");
    for (const std::string& eventId : ids)
    {
        expect(eventId.size() == 32, "generated event ID should contain 32 characters");
        for (const unsigned char ch : eventId)
        {
            expect(std::isdigit(ch) || (ch >= 'a' && ch <= 'f'),
                   "generated event ID should use lowercase hexadecimal characters");
        }
    }
}

void testAccessStatisticsResultAndBucketSemantics()
{
    expect(shortlink::isAggregatedAccessResult(shortlink::AccessEventResult::Success),
           "successful access events should be aggregated");
    expect(shortlink::isAggregatedAccessResult(shortlink::AccessEventResult::Disabled),
           "disabled access events should be aggregated for an existing short link");
    expect(!shortlink::isAggregatedAccessResult(shortlink::AccessEventResult::NotFound),
           "not-found events should not create per-code statistics");

    expect(shortlink::accessStatisticsResultIndex(shortlink::AccessEventResult::Success) == 0,
           "success should use the first statistics result slot");
    expect(shortlink::accessStatisticsResultIndex(shortlink::AccessEventResult::Error) == 3,
           "error should use the final statistics result slot");
    expect(!shortlink::accessStatisticsResultIndex(shortlink::AccessEventResult::NotFound),
           "not-found should not have a statistics result slot");

    expect(shortlink::utcHourBucketStartEpochSeconds(1784304000123) == 1784304000,
           "an event on an hour boundary should keep the UTC boundary");
    expect(shortlink::utcHourBucketStartEpochSeconds(1784307599999) == 1784304000,
           "an event within an hour should round down to the UTC hour boundary");

    bool threw = false;
    try
    {
        (void)shortlink::utcHourBucketStartEpochSeconds(0);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    expect(threw, "non-positive access event timestamps should be rejected");

    expect(shortlink::parseAccessStatisticsInterval("hour") ==
               shortlink::AccessStatisticsInterval::Hour,
           "hour statistics interval should parse");
    expect(shortlink::parseAccessStatisticsInterval("day") ==
               shortlink::AccessStatisticsInterval::Day,
           "day statistics interval should parse");
    expect(!shortlink::parseAccessStatisticsInterval("week"),
           "unsupported statistics interval should be rejected");
    expect(shortlink::accessStatisticsBucketSeconds(
               shortlink::AccessStatisticsInterval::Day) == 86400,
           "day statistics interval should use UTC day buckets");

    shortlink::AccessStatisticsQuery hourlyQuery;
    hourlyQuery.fromEpochSeconds = 1784304000;
    hourlyQuery.toEpochSeconds = 1784307600;
    hourlyQuery.interval = shortlink::AccessStatisticsInterval::Hour;
    expect(shortlink::isValidAccessStatisticsQuery(hourlyQuery),
           "aligned one-hour statistics query should be valid");
    hourlyQuery.fromEpochSeconds += 1;
    expect(!shortlink::isValidAccessStatisticsQuery(hourlyQuery),
           "unaligned statistics range should be rejected");
    hourlyQuery.fromEpochSeconds = 1784304000;
    hourlyQuery.toEpochSeconds = hourlyQuery.fromEpochSeconds + 32 * 86400;
    expect(!shortlink::isValidAccessStatisticsQuery(hourlyQuery),
           "hour statistics range over 31 days should be rejected");

    shortlink::AccessStatisticsResultCounts counts { 2, 1, 1, 3 };
    expect(shortlink::accessStatisticsAttemptCount(counts) == 7,
           "statistics attempts should sum all aggregate result categories");
}

void testDeadLetterEnvelope()
{
    shortlink::consumer::DeadLetterRecord record;
    record.sourceTopic = "hao-shortlink.access-events.v1";
    record.sourcePartition = 2;
    record.sourceOffset = 42;
    record.key = "000001";
    record.payload = "not-json";
    record.reason = shortlink::consumer::DeadLetterReason::InvalidJson;

    const std::string payload = shortlink::consumer::serializeDeadLetterRecord(
        record,
        1784304000123);
    expect(payload.find("\"schema_version\":1") != std::string::npos,
           "DLQ envelope should be versioned");
    expect(payload.find("\"reason\":\"invalid_json\"") != std::string::npos,
           "DLQ envelope should retain a fixed reason");
    expect(payload.find("\"source_partition\":2") != std::string::npos,
           "DLQ envelope should retain the source partition");
    expect(payload.find("\"source_offset\":42") != std::string::npos,
           "DLQ envelope should retain the source offset");
    expect(payload.find("\"original_key_base64\":\"MDAwMDAx\"") != std::string::npos,
           "DLQ envelope should safely encode the original key");
    expect(payload.find("\"original_payload_base64\":\"bm90LWpzb24=\"") !=
               std::string::npos,
           "DLQ envelope should safely encode the original payload");
}

void testConsumerMetricsRendering()
{
    shortlink::consumer::ConsumerMetrics metrics;
    metrics.recordMessage(
        shortlink::consumer::ConsumerMetrics::MessageResult::Aggregated);
    metrics.recordMessage(
        shortlink::consumer::ConsumerMetrics::MessageResult::Duplicate);
    metrics.recordRetry(
        shortlink::consumer::ConsumerMetrics::RetryOperation::Mysql);
    metrics.recordDlq(shortlink::consumer::ConsumerMetrics::DlqResult::Failure);
    metrics.replaceLag({ { 0, 3 }, { 2, 7 } });
    metrics.recordSuccess();
    metrics.setHealthy(true);

    const std::string rendered = metrics.renderPrometheus();
    expect(rendered.find(
               "haohttp_shortlink_access_consumer_messages_total{result=\"aggregated\"} 1") !=
               std::string::npos,
           "consumer metrics should render aggregated messages");
    expect(rendered.find(
               "haohttp_shortlink_access_consumer_messages_total{result=\"duplicate\"} 1") !=
               std::string::npos,
           "consumer metrics should render duplicate messages");
    expect(rendered.find(
               "haohttp_shortlink_access_consumer_retries_total{operation=\"mysql\"} 1") !=
               std::string::npos,
           "consumer metrics should render bounded retry labels");
    expect(rendered.find(
               "haohttp_shortlink_access_consumer_dlq_total{result=\"failure\"} 1") !=
               std::string::npos,
           "consumer metrics should render DLQ delivery failures");
    expect(rendered.find(
               "haohttp_shortlink_access_consumer_lag{partition=\"2\"} 7") !=
               std::string::npos,
           "consumer metrics should expose lag only by partition");
    expect(rendered.find("haohttp_shortlink_access_consumer_last_success_unixtime 0") ==
               std::string::npos,
           "consumer metrics should expose the last successful commit time");
    expect(metrics.healthy(), "consumer health should be independently observable");
}

class RecordingAccessEventPublisher : public shortlink::AccessEventPublisher
{
public:
    void publish(const shortlink::AccessEvent& event) noexcept override
    {
        lastEvent = event;
        ++publishCalls;
    }

    shortlink::AccessEvent lastEvent;
    int publishCalls { 0 };
};

void testAccessEventPublisherAbstraction()
{
    const shortlink::AccessEvent event {
        shortlink::generateAccessEventId(),
        shortlink::nowEpochMilliseconds(),
        "publisher-test-request",
        "000001",
        shortlink::AccessEventResult::Success,
        302
    };

    RecordingAccessEventPublisher recording;
    recording.publish(event);
    expect(recording.publishCalls == 1, "publisher abstraction should receive access event");
    expect(recording.lastEvent.eventId == event.eventId,
           "publisher abstraction should preserve access event");

    shortlink::NoopAccessEventPublisher noop;
    noop.publish(event);
}

void testHttpMetricsPrometheusRendering()
{
    http::metrics::HttpMetrics metrics;
    metrics.registerRoute(http::HttpRequest::kGet, "/api/health");
    metrics.record(http::HttpRequest::kGet,
                   "/api/health",
                   http::HttpResponse::k200Ok,
                   0.0004);
    metrics.record(http::HttpRequest::kGet,
                   "/api/health",
                   http::HttpResponse::k500InternalServerError,
                   0.003);

    const std::string rendered = metrics.renderPrometheus();
    expect(rendered.find("haohttp_http_requests_total{method=\"GET\",route=\"/api/health\",status_class=\"2xx\"} 1") !=
               std::string::npos,
           "HTTP metrics should count successful requests");
    expect(rendered.find("haohttp_http_requests_total{method=\"GET\",route=\"/api/health\",status_class=\"5xx\"} 1") !=
               std::string::npos,
           "HTTP metrics should count server errors");
    expect(rendered.find("haohttp_http_request_duration_seconds_bucket{method=\"GET\",route=\"/api/health\",le=\"0.0005\"} 1") !=
               std::string::npos,
           "HTTP histogram should place fast request in first bucket");
    expect(rendered.find("haohttp_http_request_duration_seconds_bucket{method=\"GET\",route=\"/api/health\",le=\"0.005\"} 2") !=
               std::string::npos,
           "HTTP histogram buckets should be cumulative");
    expect(rendered.find("haohttp_http_request_duration_seconds_bucket{method=\"GET\",route=\"/api/health\",le=\"+Inf\"} 2") !=
               std::string::npos,
           "HTTP histogram infinite bucket should include every request");
    expect(rendered.find("haohttp_http_request_duration_seconds_count{method=\"GET\",route=\"/api/health\"} 2") !=
               std::string::npos,
           "HTTP histogram should expose the observation count");
}

void testHttpMetricsConcurrentUpdates()
{
    constexpr int threadCount = 8;
    constexpr int recordsPerThread = 1000;
    http::metrics::HttpMetrics metrics;
    metrics.registerRoute(http::HttpRequest::kGet, "/s/:code");
    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i)
    {
        threads.emplace_back([&metrics]() {
            for (int j = 0; j < recordsPerThread; ++j)
            {
                metrics.record(http::HttpRequest::kGet,
                               "/s/:code",
                               http::HttpResponse::k302Found,
                               0.001);
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    const std::string expected =
        "haohttp_http_requests_total{method=\"GET\",route=\"/s/:code\",status_class=\"3xx\"} " +
        std::to_string(threadCount * recordsPerThread);
    expect(metrics.renderPrometheus().find(expected) != std::string::npos,
           "concurrent HTTP metric updates should not lose requests");
}

void testShortLinkMetricsPrometheusRendering()
{
    shortlink::ShortLinkMetrics metrics;
    metrics.recordCreate(shortlink::ShortLinkMetrics::CreateResult::Success,
                         shortlink::ShortLinkMetrics::Storage::Memory);
    metrics.recordRedirect(shortlink::ShortLinkMetrics::RedirectResult::NotFound,
                           shortlink::ShortLinkMetrics::RedirectSource::Mysql);
    metrics.recordCache(shortlink::ShortLinkMetrics::CacheOperation::Get,
                        shortlink::ShortLinkMetrics::CacheResult::Miss);
    metrics.recordBackendError(shortlink::ShortLinkMetrics::Backend::Redis,
                               shortlink::ShortLinkMetrics::BackendOperation::Get);
    metrics.recordBackendError(shortlink::ShortLinkMetrics::Backend::Mysql,
                               shortlink::ShortLinkMetrics::BackendOperation::Update);
    metrics.recordRateLimit(shortlink::ShortLinkMetrics::RateLimitResult::Limited);
    metrics.recordAccessEventEnqueue(
        shortlink::ShortLinkMetrics::AccessEventEnqueueResult::Accepted);
    metrics.recordAccessEventEnqueue(
        shortlink::ShortLinkMetrics::AccessEventEnqueueResult::QueueFull);
    metrics.recordAccessEventDelivery(
        shortlink::ShortLinkMetrics::AccessEventDeliveryResult::Failure);
    metrics.setAccessEventQueueSize(7);

    const std::string rendered = metrics.renderPrometheus();
    expect(rendered.find("haohttp_shortlink_create_total{result=\"success\",storage=\"memory\"} 1") !=
               std::string::npos,
           "short link metrics should count successful memory creates");
    expect(rendered.find("haohttp_shortlink_redirect_total{result=\"not_found\",source=\"mysql\"} 1") !=
               std::string::npos,
           "short link metrics should count missing MySQL redirects");
    expect(rendered.find("haohttp_shortlink_cache_operations_total{operation=\"get\",result=\"miss\"} 1") !=
               std::string::npos,
           "short link metrics should count Redis misses");
    expect(rendered.find("haohttp_shortlink_backend_errors_total{backend=\"redis\",operation=\"get\"} 1") !=
               std::string::npos,
           "short link metrics should count Redis get errors");
    expect(rendered.find("haohttp_shortlink_backend_errors_total{backend=\"mysql\",operation=\"update\"} 1") !=
               std::string::npos,
           "short link metrics should distinguish MySQL lifecycle update errors");
    expect(rendered.find("haohttp_shortlink_rate_limit_checks_total{result=\"limited\"} 1") !=
               std::string::npos,
           "short link metrics should count limited creates");
    expect(rendered.find("haohttp_shortlink_access_event_enqueue_total{result=\"accepted\"} 1") !=
               std::string::npos,
           "short link metrics should count accepted access events");
    expect(rendered.find("haohttp_shortlink_access_event_enqueue_total{result=\"queue_full\"} 1") !=
               std::string::npos,
           "short link metrics should count full Kafka producer queues");
    expect(rendered.find("haohttp_shortlink_access_event_delivery_total{result=\"failure\"} 1") !=
               std::string::npos,
           "short link metrics should count Kafka delivery failures");
    expect(rendered.find("haohttp_shortlink_access_event_queue_size 7") != std::string::npos,
           "short link metrics should expose the Kafka producer queue size");
}

void testShortLinkMetricsConcurrentUpdates()
{
    constexpr int threadCount = 8;
    constexpr int recordsPerThread = 1000;
    shortlink::ShortLinkMetrics metrics;
    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i)
    {
        threads.emplace_back([&metrics]() {
            for (int j = 0; j < recordsPerThread; ++j)
            {
                metrics.recordCreate(shortlink::ShortLinkMetrics::CreateResult::Success,
                                     shortlink::ShortLinkMetrics::Storage::Memory);
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    const std::string expected =
        "haohttp_shortlink_create_total{result=\"success\",storage=\"memory\"} " +
        std::to_string(threadCount * recordsPerThread);
    expect(metrics.renderPrometheus().find(expected) != std::string::npos,
           "concurrent short link metric updates should not lose operations");
}

void testResponseJsonErrorEscapesBody()
{
    http::HttpResponse resp(false);
    resp.setErrorResponse(http::HttpResponse::k400BadRequest, "bad\"code", "Bad \"Request\"");

    const std::string raw = responseToString(resp);
    expect(raw.find("HTTP/1.1 400 Bad \"Request\"\r\n") != std::string::npos,
           "error response should use provided status message");
    expect(raw.find("Connection: Keep-Alive\r\n") != std::string::npos,
           "non-closing response should keep connection alive");
    expect(raw.find("Content-Type: application/json; charset=utf-8\r\n") != std::string::npos,
           "JSON error response should set content type");
    expect(raw.find("{\"error\":{\"code\":\"bad\\\"code\",\"message\":\"Bad \\\"Request\\\"\"}}") != std::string::npos,
           "JSON error response should escape body fields");
}

void testResponseRedirect()
{
    http::HttpResponse resp(false);
    resp.setRedirect("https://example.com/target");

    const std::string raw = responseToString(resp);
    expect(raw.find("HTTP/1.1 302 Found\r\n") != std::string::npos,
           "redirect should set 302 status line");
    expect(raw.find("Location: https://example.com/target\r\n") != std::string::npos,
           "redirect should set Location header");
    expect(raw.find("Content-Length: 0\r\n") != std::string::npos,
           "redirect should set empty body length");
}

class RecordingMiddleware : public http::middleware::Middleware
{
public:
    RecordingMiddleware(std::string name, std::vector<std::string>* events)
        : name_(std::move(name)),
          events_(events)
    {}

    void before(http::HttpRequest& request) override
    {
        (void)request;
        events_->push_back("before-" + name_);
    }

    void after(http::HttpResponse& response) override
    {
        (void)response;
        events_->push_back("after-" + name_);
    }

private:
    std::string name_;
    std::vector<std::string>* events_;
};

void testMiddlewareOrder()
{
    std::vector<std::string> events;
    http::middleware::MiddlewareChain chain;
    chain.addMiddleware(std::make_shared<RecordingMiddleware>("first", &events));
    chain.addMiddleware(std::make_shared<RecordingMiddleware>("second", &events));

    http::HttpRequest req = makeRequest(http::HttpRequest::kGet, "/api/health");
    http::HttpResponse resp(false);
    chain.processBefore(req);
    chain.processAfter(resp);

    const std::vector<std::string> expected {
        "before-first",
        "before-second",
        "after-second",
        "after-first"
    };
    expect(events == expected, "middleware before should run forward and after should run backward");
}

class CountingRepository : public shortlink::ShortLinkRepository
{
public:
    std::optional<ShortLinkRecord> create(
        const std::string& originalUrl,
        std::optional<std::int64_t> expiresAt = std::nullopt,
        std::uint64_t ownerId = 1,
        std::optional<std::string> customCode = std::nullopt) override
    {
        ++createCalls;
        return ShortLinkRecord {
            1,
            ownerId,
            customCode.value_or("custom01"),
            originalUrl,
            Status::Active,
            expiresAt,
            1,
            1
        };
    }

    LookupResult findByCode(const std::string& code) const override
    {
        if (code == "custom01")
        {
            return { ShortLinkRecord { 1,
                                       1,
                                       "custom01",
                                       "https://example.com/custom",
                                       Status::Active,
                                       std::nullopt,
                                       1,
                                       1 },
                     LookupSource::Memory };
        }

        return { std::nullopt, LookupSource::Memory };
    }

    std::optional<ShortLinkRecord> findByCodeForOwner(
        const std::string& code,
        std::uint64_t ownerId) const override
    {
        const LookupResult result = findByCode(code);
        return result.record && result.record->ownerId == ownerId
                   ? result.record
                   : std::nullopt;
    }

    LookupSource defaultLookupSource() const noexcept override
    {
        return LookupSource::Memory;
    }

    std::vector<ShortLinkRecord> list(const ListQuery&) const override { return {}; }

    std::optional<ShortLinkRecord> updateLifecycle(
        const std::string&,
        const LifecycleUpdate&) override
    {
        return std::nullopt;
    }

    int createCalls { 0 };
};

class FailingLookupRepository : public shortlink::ShortLinkRepository
{
public:
    std::optional<ShortLinkRecord> create(
        const std::string&,
        std::optional<std::int64_t> = std::nullopt,
        std::uint64_t = 1,
        std::optional<std::string> = std::nullopt) override
    {
        return std::nullopt;
    }

    std::optional<ShortLinkRecord> findByCodeForOwner(
        const std::string&,
        std::uint64_t) const override
    {
        throw std::runtime_error("lookup failed");
    }

    LookupResult findByCode(const std::string&) const override
    {
        throw std::runtime_error("lookup failed");
    }

    LookupSource defaultLookupSource() const noexcept override
    {
        return LookupSource::Mysql;
    }

    std::vector<ShortLinkRecord> list(const ListQuery&) const override { return {}; }

    std::optional<ShortLinkRecord> updateLifecycle(
        const std::string&,
        const LifecycleUpdate&) override
    {
        return std::nullopt;
    }
};

void testRedirectHandlerPublishesErrorAndRethrows()
{
    FailingLookupRepository repository;
    shortlink::ShortLinkService service(repository);
    RecordingAccessEventPublisher publisher;
    http::HttpRequest request = makeRequest(http::HttpRequest::kGet, "/s/failing");
    request.setPathParameters("param1", "failing");
    request.setRequestId("redirect-error-request");
    http::HttpResponse response(false);

    bool threw = false;
    try
    {
        shortlink::handleRedirect(request, &response, &service, &publisher);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    expect(threw, "redirect handler should preserve the HTTP error boundary");
    expect(publisher.publishCalls == 1, "redirect lookup failure should publish one event");
    expect(publisher.lastEvent.result == shortlink::AccessEventResult::Error,
           "redirect lookup failure should publish an error event");
    expect(publisher.lastEvent.httpStatus == 500,
           "redirect lookup failure event should describe the final HTTP status");
    expect(publisher.lastEvent.requestId == "redirect-error-request",
           "redirect error event should preserve the request ID");
    expect(publisher.lastEvent.code == "failing",
           "redirect error event should preserve the short code");
}

void testShortLinkServiceUrlValidation()
{
    shortlink::MemoryShortLinkRepository repository;
    shortlink::ShortLinkService service(repository);

    expect(service.isValidUrl("http://example.com"), "http URL should be valid");
    expect(service.isValidUrl("https://example.com/path"), "https URL should be valid");
    expect(!service.isValidUrl("ftp://example.com"), "ftp URL should be invalid");
    expect(!service.isValidUrl("example.com"), "URL without supported scheme should be invalid");
    expect(!service.isValidUrl(""), "empty URL should be invalid");
}

void testShortLinkServiceCreatesAndFindsMemoryLink()
{
    shortlink::MemoryShortLinkRepository repository;
    shortlink::ShortLinkService service(repository);

    const std::string originalUrl = "https://example.com/very/long/path";
    const std::optional<shortlink::ShortLinkService::ShortLink> shortLink =
        service.createShortLink(originalUrl);

    expect(shortLink.has_value(), "valid URL should create a short link");
    expect(shortLink->record.code == "000001", "first memory short code should be deterministic");
    expect(shortLink->shortUrl == "/s/000001", "short URL should be based on code");
    expect(shortLink->record.originalUrl == originalUrl, "short link should keep original URL");

    const shortlink::ShortLinkService::RedirectResult found = service.resolve(shortLink->record.code);
    expect(found.status == shortlink::ShortLinkService::RedirectStatus::Success,
           "created code should be findable");
    expect(found.originalUrl == originalUrl, "created code should resolve to original URL");
}

void testMemoryRepositoryCreatesUniqueCodes()
{
    shortlink::MemoryShortLinkRepository repository;

    const std::optional<shortlink::ShortLinkRepository::ShortLinkRecord> first =
        repository.create("https://example.com/one");
    const std::optional<shortlink::ShortLinkRepository::ShortLinkRecord> second =
        repository.create("https://example.com/two");

    expect(first.has_value(), "first memory repository create should succeed");
    expect(second.has_value(), "second memory repository create should succeed");
    expect(first->code == "000001", "first memory code should be 000001");
    expect(second->code == "000002", "second memory code should be 000002");
    expect(first->code != second->code, "memory repository should generate unique codes");
}

void testShortLinkServiceRejectsInvalidUrlWithoutRepositoryWrite()
{
    CountingRepository repository;
    shortlink::ShortLinkService service(repository);

    const std::optional<shortlink::ShortLinkService::ShortLink> shortLink =
        service.createShortLink("ftp://example.com/not-supported");

    expect(!shortLink.has_value(), "invalid URL should not create a short link");
    expect(repository.createCalls == 0, "invalid URL should not call repository create");
}

void testRedirectErrorMetricIsNotRecordedForInternalLookup()
{
    FailingLookupRepository repository;
    shortlink::ShortLinkMetrics metrics;
    shortlink::ShortLinkService service(repository, &metrics);

    try
    {
        service.resolve("failure");
        throw TestFailure("redirect lookup failure should propagate");
    }
    catch (const TestFailure&)
    {
        throw;
    }
    catch (const std::runtime_error&)
    {
    }

    try
    {
        service.get("failure");
        throw TestFailure("internal lookup failure should propagate");
    }
    catch (const TestFailure&)
    {
        throw;
    }
    catch (const std::runtime_error&)
    {
    }

    const std::string rendered = metrics.renderPrometheus();
    expect(rendered.find("haohttp_shortlink_redirect_total{result=\"error\",source=\"mysql\"} 1") !=
               std::string::npos,
           "only redirect lookup should record redirect error metric");
}

void testMemoryRepositoryMissingCode()
{
    shortlink::MemoryShortLinkRepository repository;
    repository.create("https://example.com/existing");

    const shortlink::ShortLinkRepository::LookupResult missing = repository.findByCode("missing");
    expect(!missing.record.has_value(), "missing memory code should return no record");
}

void testShortLinkLifecycleDisabledAndExpired()
{
    shortlink::MemoryShortLinkRepository repository;
    shortlink::ShortLinkMetrics metrics;
    shortlink::ShortLinkService service(repository, &metrics);

    const std::optional<shortlink::ShortLinkService::ShortLink> created =
        service.createShortLink("https://example.com/lifecycle");
    expect(created.has_value(), "lifecycle link should be created");

    shortlink::ShortLinkRepository::LifecycleUpdate disable;
    disable.status = shortlink::ShortLinkRepository::Status::Disabled;
    expect(service.updateLifecycle(created->record.code, disable).has_value(),
           "existing link should be disabled");
    expect(service.resolve(created->record.code).status ==
               shortlink::ShortLinkService::RedirectStatus::Disabled,
           "disabled link should not resolve");

    shortlink::ShortLinkRepository::LifecycleUpdate expire;
    expire.status = shortlink::ShortLinkRepository::Status::Active;
    expire.expiresAtProvided = true;
    expire.expiresAt = shortlink::ShortLinkService::nowEpochSeconds() - 1;
    expect(service.updateLifecycle(created->record.code, expire).has_value(),
           "existing link should accept lifecycle update");
    expect(service.resolve(created->record.code).status ==
               shortlink::ShortLinkService::RedirectStatus::Expired,
           "expired link should not resolve");

    const std::string rendered = metrics.renderPrometheus();
    expect(rendered.find("result=\"disabled\",source=\"memory\"} 1") != std::string::npos,
           "metrics should distinguish disabled redirects");
    expect(rendered.find("result=\"expired\",source=\"memory\"} 1") != std::string::npos,
           "metrics should distinguish expired redirects");
}

void testMemoryRepositoryLifecycleList()
{
    shortlink::MemoryShortLinkRepository repository;
    const auto first = repository.create("https://example.com/one");
    const auto second = repository.create("https://example.com/two");
    const auto third = repository.create("https://example.com/three");
    expect(first && second && third, "list fixtures should be created");

    shortlink::ShortLinkRepository::LifecycleUpdate disable;
    disable.status = shortlink::ShortLinkRepository::Status::Disabled;
    repository.updateLifecycle(second->code, disable);

    shortlink::ShortLinkRepository::ListQuery firstPage;
    firstPage.limit = 2;
    const auto page = repository.list(firstPage);
    expect(page.size() == 2 && page[0].id == first->id && page[1].id == second->id,
           "memory list should be stable and ordered by id");

    shortlink::ShortLinkRepository::ListQuery activePage;
    activePage.cursor = first->id;
    activePage.limit = 10;
    activePage.status = shortlink::ShortLinkRepository::Status::Active;
    const auto active = repository.list(activePage);
    expect(active.size() == 1 && active[0].id == third->id,
           "memory list should support cursor and status filter");
}

void testPasswordHasherRoundTrip()
{
    const auto encoded = shortlink::PasswordHasher::hash("correct-horse-battery");
    expect(encoded.has_value(), "password hashing should succeed");
    expect(encoded->find("scrypt$") == 0, "password hash should record the scrypt format");
    expect(shortlink::PasswordHasher::verify("correct-horse-battery", *encoded),
           "password verifier should accept the original password");
    expect(!shortlink::PasswordHasher::verify("wrong-password", *encoded),
           "password verifier should reject a different password");
    expect(!shortlink::PasswordHasher::verify("correct-horse-battery", "invalid"),
           "password verifier should reject a malformed hash");
}

void testAuthServiceRegistrationSessionAndLogout()
{
    shortlink::MemoryAuthRepository repository;
    shortlink::AuthService service(repository, 3600);

    const auto registered = service.registerUser("Alice_1", "a-secure-password");
    expect(registered.status == shortlink::AuthService::ResultStatus::Success &&
               registered.session.has_value(),
           "valid credentials should register and create a session");
    expect(registered.session->user.username == "Alice_1",
           "registration should preserve the display username");
    expect(registered.session->token.size() == 64,
           "session token should contain 32 random bytes encoded as hex");
    expect(shortlink::AuthService::tokenHash(registered.session->token) !=
               registered.session->token,
           "the persisted session key should be a digest rather than the raw token");
    expect(service.authenticate(registered.session->token).has_value(),
           "new session should authenticate");

    expect(service.registerUser("9invalid", "a-secure-password").status ==
               shortlink::AuthService::ResultStatus::InvalidUsername,
           "username should begin with an ASCII letter");
    expect(service.registerUser("\xc3\xa9-user", "a-secure-password").status ==
               shortlink::AuthService::ResultStatus::InvalidUsername,
           "username should reject non-ASCII letters");
    expect(service.registerUser("ValidUser", "short").status ==
               shortlink::AuthService::ResultStatus::InvalidPassword,
           "password shorter than the minimum should be rejected");

    const auto conflict = service.registerUser("alice_1", "another-password");
    expect(conflict.status == shortlink::AuthService::ResultStatus::UsernameConflict,
           "normalized usernames should be unique");
    const auto invalidLogin = service.login("Alice_1", "wrong-password");
    expect(invalidLogin.status == shortlink::AuthService::ResultStatus::InvalidCredentials,
           "wrong password should return the unified credential error");

    service.logout(registered.session->token);
    expect(!service.authenticate(registered.session->token).has_value(),
           "logout should revoke the current session");

    const auto loggedIn = service.login("ALICE_1", "a-secure-password");
    expect(loggedIn.status == shortlink::AuthService::ResultStatus::Success,
           "login should use the normalized username");

    shortlink::MemoryAuthRepository expiringRepository;
    shortlink::AuthService expiringService(expiringRepository, 0);
    const auto expired = expiringService.registerUser("ExpiredUser", "a-secure-password");
    expect(expired.session && !expiringService.authenticate(expired.session->token),
           "a session at its absolute expiry must be rejected");
}

void testCustomCodeAndOwnerIsolation()
{
    shortlink::MemoryShortLinkRepository repository;
    shortlink::ShortLinkService service(repository);

    const auto created = service.createShortLink(
        "https://example.com/owned", std::nullopt, 42, std::string("Owned_Code"));
    expect(created && created->record.code == "Owned_Code" && created->record.ownerId == 42,
           "custom code should preserve code and owner");
    expect(service.getForOwner("Owned_Code", 42).has_value(),
           "owner should be able to read the link");
    expect(!service.getForOwner("Owned_Code", 43).has_value(),
           "a different owner should not read the link");
    expect(!service.createShortLink(
               "https://example.com/reserved", std::nullopt, 42, std::string("api")),
           "reserved custom code should be rejected");

    bool conflict = false;
    try
    {
        service.createShortLink(
            "https://example.com/conflict", std::nullopt, 43, std::string("Owned_Code"));
    }
    catch (const shortlink::ShortLinkRepository::ShortCodeConflict&)
    {
        conflict = true;
    }
    expect(conflict, "custom code uniqueness should apply across owners");

    shortlink::ShortLinkRepository::ListQuery ownerQuery;
    ownerQuery.ownerId = 43;
    expect(repository.list(ownerQuery).empty(),
           "owner-scoped list should not expose another user's link");
}

void testUtcTimestampParsing()
{
    const auto leapDay = shortlink::ShortLinkService::parseUtcTimestamp("2028-02-29T12:34:56Z");
    expect(leapDay.has_value(), "valid leap-day UTC timestamp should parse");
    expect(shortlink::ShortLinkService::formatUtcTimestamp(*leapDay) == "2028-02-29T12:34:56Z",
           "UTC timestamp should round trip");
    expect(!shortlink::ShortLinkService::parseUtcTimestamp("2027-02-29T12:34:56Z"),
           "invalid calendar date should be rejected");
    expect(!shortlink::ShortLinkService::parseUtcTimestamp("2028-02-29T12:34:56+08:00"),
           "non-UTC timestamp should be rejected by v1.7 format");
}

} // namespace

int main()
{
    const std::vector<TestCase> tests {
        {"router exact callback", testRouterExactCallback},
        {"router dynamic callback path parameters", testRouterDynamicCallbackPathParameters},
        {"router no match", testRouterNoMatch},
        {"request swap clears body state", testRequestSwapClearsBodyState},
        {"request header lookup is case insensitive", testRequestHeaderLookupIsCaseInsensitive},
        {"same-origin state-change policy", testSameOriginPolicy},
        {"request ID validation and generation", testRequestIdValidationAndGeneration},
        {"request ID concurrent generation", testRequestIdConcurrentGeneration},
        {"access event round trip", testAccessEventRoundTrip},
        {"access event rejects invalid payloads", testAccessEventRejectsInvalidPayloads},
        {"access event ID generation", testAccessEventIdGeneration},
        {"access statistics result and bucket semantics", testAccessStatisticsResultAndBucketSemantics},
        {"dead-letter envelope", testDeadLetterEnvelope},
        {"consumer metrics rendering", testConsumerMetricsRendering},
        {"access event publisher abstraction", testAccessEventPublisherAbstraction},
        {"redirect handler publishes error and rethrows", testRedirectHandlerPublishesErrorAndRethrows},
        {"HTTP metrics Prometheus rendering", testHttpMetricsPrometheusRendering},
        {"HTTP metrics concurrent updates", testHttpMetricsConcurrentUpdates},
        {"short link metrics Prometheus rendering", testShortLinkMetricsPrometheusRendering},
        {"short link metrics concurrent updates", testShortLinkMetricsConcurrentUpdates},
        {"response JSON error escapes body", testResponseJsonErrorEscapesBody},
        {"response redirect", testResponseRedirect},
        {"middleware order", testMiddlewareOrder},
        {"shortlink service URL validation", testShortLinkServiceUrlValidation},
        {"shortlink service creates and finds memory link", testShortLinkServiceCreatesAndFindsMemoryLink},
        {"memory repository creates unique codes", testMemoryRepositoryCreatesUniqueCodes},
        {"shortlink service rejects invalid URL without repository write", testShortLinkServiceRejectsInvalidUrlWithoutRepositoryWrite},
        {"redirect error metric excludes internal lookup", testRedirectErrorMetricIsNotRecordedForInternalLookup},
        {"memory repository missing code", testMemoryRepositoryMissingCode},
        {"shortlink lifecycle disabled and expired", testShortLinkLifecycleDisabledAndExpired},
        {"memory repository lifecycle list", testMemoryRepositoryLifecycleList},
        {"password hasher round trip", testPasswordHasherRoundTrip},
        {"auth registration session and logout", testAuthServiceRegistrationSessionAndLogout},
        {"custom code and owner isolation", testCustomCodeAndOwnerIsolation},
        {"UTC timestamp parsing", testUtcTimestampParsing}
    };

    int failed = 0;
    for (const TestCase& test : tests)
    {
        try
        {
            test.run();
            std::cout << "PASS: " << test.name << std::endl;
        }
        catch (const std::exception& e)
        {
            ++failed;
            std::cout << "FAIL: " << test.name << " - " << e.what() << std::endl;
        }
    }

    if (failed > 0)
    {
        std::cerr << failed << " test(s) failed" << std::endl;
        return 1;
    }

    std::cout << tests.size() << " test(s) passed" << std::endl;
    return 0;
}
