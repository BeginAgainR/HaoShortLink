#pragma once

#include "shortlink/AuthHttp.h"
#include "shortlink/ShortLinkMetrics.h"

namespace http
{
class HttpServer;
}

namespace shortlink
{

class AccessEventPublisher;
class AccessStatisticsRepository;
class AuthService;
class RateLimiter;
class ShortLinkService;

struct ShortLinkHttpApiConfig
{
    bool requiresMySql { false };
    bool metricsEnabled { true };
    ShortLinkMetrics::Storage metricsStorage { ShortLinkMetrics::Storage::Memory };
    AuthHttpConfig auth;
};

void registerShortLinkHttpApi(http::HttpServer* server,
                              ShortLinkService* shortLinkService,
                              AuthService* authService,
                              ShortLinkMetrics* metrics,
                              const RateLimiter* rateLimiter,
                              AccessEventPublisher* accessEventPublisher,
                              const AccessStatisticsRepository* statisticsRepository,
                              ShortLinkHttpApiConfig config);

} // namespace shortlink
