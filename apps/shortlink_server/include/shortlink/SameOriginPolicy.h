#pragma once

namespace http
{
class HttpRequest;
class HttpResponse;
}

namespace shortlink
{

bool isSameOriginRequest(const http::HttpRequest& request);
bool requireSameOriginRequest(const http::HttpRequest& request,
                              http::HttpResponse* response);

} // namespace shortlink
