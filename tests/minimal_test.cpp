#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "middleware/MiddlewareChain.h"
#include "router/Router.h"
#include "shortlink/MemoryShortLinkRepository.h"
#include "shortlink/ShortLinkService.h"
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
    std::optional<ShortLinkRecord> create(const std::string& originalUrl) override
    {
        ++createCalls;
        return ShortLinkRecord {
            "custom01",
            originalUrl
        };
    }

    std::optional<std::string> findOriginalUrl(const std::string& code) const override
    {
        if (code == "custom01")
        {
            return std::string("https://example.com/custom");
        }

        return std::nullopt;
    }

    int createCalls { 0 };
};

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
    expect(shortLink->code == "000001", "first memory short code should be deterministic");
    expect(shortLink->shortUrl == "/s/000001", "short URL should be based on code");
    expect(shortLink->originalUrl == originalUrl, "short link should keep original URL");

    const std::optional<std::string> found = service.findOriginalUrl(shortLink->code);
    expect(found.has_value(), "created code should be findable");
    expect(*found == originalUrl, "created code should resolve to original URL");
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

void testMemoryRepositoryMissingCode()
{
    shortlink::MemoryShortLinkRepository repository;
    repository.create("https://example.com/existing");

    const std::optional<std::string> missing = repository.findOriginalUrl("missing");
    expect(!missing.has_value(), "missing memory code should return no URL");
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
        {"request ID validation and generation", testRequestIdValidationAndGeneration},
        {"request ID concurrent generation", testRequestIdConcurrentGeneration},
        {"response JSON error escapes body", testResponseJsonErrorEscapesBody},
        {"response redirect", testResponseRedirect},
        {"middleware order", testMiddlewareOrder},
        {"shortlink service URL validation", testShortLinkServiceUrlValidation},
        {"shortlink service creates and finds memory link", testShortLinkServiceCreatesAndFindsMemoryLink},
        {"memory repository creates unique codes", testMemoryRepositoryCreatesUniqueCodes},
        {"shortlink service rejects invalid URL without repository write", testShortLinkServiceRejectsInvalidUrlWithoutRepositoryWrite},
        {"memory repository missing code", testMemoryRepositoryMissingCode}
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
