#include "shortlink/SameOriginPolicy.h"

#include "http/HttpRequest.h"
#include "http/HttpResponse.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace shortlink
{

namespace
{

std::string asciiLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a')
                                      : static_cast<char>(ch);
    });
    return value;
}

bool isValidAuthority(const std::string& authority)
{
    if (authority.empty())
    {
        return false;
    }
    return std::none_of(authority.begin(), authority.end(), [](unsigned char ch) {
        return std::isspace(ch) || ch == '/' || ch == '?' || ch == '#' || ch == '@';
    });
}

} // namespace

bool isSameOriginRequest(const http::HttpRequest& request)
{
    const std::string fetchSite = asciiLower(request.getHeader("Sec-Fetch-Site"));
    if (fetchSite == "cross-site")
    {
        return false;
    }

    const std::string origin = request.getHeader("Origin");
    if (origin.empty())
    {
        // Non-browser clients commonly omit Origin. Browser cross-site state-changing
        // requests provide Origin and/or Sec-Fetch-Site, which are checked above.
        return true;
    }

    const std::string host = request.getHeader("Host");
    if (!isValidAuthority(host))
    {
        return false;
    }

    std::string scheme = asciiLower(request.getHeader("X-Forwarded-Proto"));
    if (scheme.empty())
    {
        scheme = "http";
    }
    if (scheme != "http" && scheme != "https")
    {
        return false;
    }

    return asciiLower(origin) == scheme + "://" + asciiLower(host);
}

bool requireSameOriginRequest(const http::HttpRequest& request,
                              http::HttpResponse* response)
{
    response->addHeader("Cache-Control", "no-store");
    if (isSameOriginRequest(request))
    {
        return true;
    }
    response->setErrorResponse(http::HttpResponse::k403Forbidden,
                               "origin_not_allowed",
                               "Request origin is not allowed");
    return false;
}

} // namespace shortlink
