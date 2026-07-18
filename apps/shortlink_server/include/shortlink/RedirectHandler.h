#pragma once

namespace http
{
class HttpRequest;
class HttpResponse;
}

namespace shortlink
{

class AccessEventPublisher;
class ShortLinkService;

void handleRedirect(const http::HttpRequest& request,
                    http::HttpResponse* response,
                    ShortLinkService* service,
                    AccessEventPublisher* eventPublisher);

} // namespace shortlink
