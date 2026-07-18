#include "shortlink/RedirectHandler.h"

#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "shortlink/AccessEvent.h"
#include "shortlink/AccessEventPublisher.h"
#include "shortlink/ShortLinkService.h"

#include <exception>
#include <string>

#include <muduo/base/Logging.h>

namespace shortlink
{

namespace
{

void publishAccessEvent(AccessEventPublisher* publisher,
                        const http::HttpRequest& request,
                        const std::string& code,
                        AccessEventResult result,
                        int httpStatus) noexcept
{
    try
    {
        publisher->publish(AccessEvent {
            generateAccessEventId(),
            nowEpochMilliseconds(),
            request.requestId(),
            code,
            result,
            httpStatus
        });
    }
    catch (const std::exception& error)
    {
        LOG_ERROR << "event=kafka_access_event stage=construct result=failure"
                  << " request_id=" << request.requestId()
                  << " reason=" << error.what();
    }
    catch (...)
    {
        LOG_ERROR << "event=kafka_access_event stage=construct result=failure"
                  << " request_id=" << request.requestId()
                  << " reason=unknown_error";
    }
}

} // namespace

void handleRedirect(const http::HttpRequest& request,
                    http::HttpResponse* response,
                    ShortLinkService* service,
                    AccessEventPublisher* eventPublisher)
{
    const std::string code = request.getPathParameters("param1");
    ShortLinkService::RedirectResult result;
    try
    {
        result = service->resolve(code);
    }
    catch (...)
    {
        publishAccessEvent(eventPublisher, request, code, AccessEventResult::Error, 500);
        throw;
    }

    AccessEventResult eventResult = AccessEventResult::NotFound;
    int httpStatus = 404;
    switch (result.status)
    {
    case ShortLinkService::RedirectStatus::Success:
        eventResult = AccessEventResult::Success;
        httpStatus = 302;
        break;
    case ShortLinkService::RedirectStatus::Disabled:
        eventResult = AccessEventResult::Disabled;
        break;
    case ShortLinkService::RedirectStatus::Expired:
        eventResult = AccessEventResult::Expired;
        break;
    case ShortLinkService::RedirectStatus::NotFound:
        eventResult = AccessEventResult::NotFound;
        break;
    }

    publishAccessEvent(eventPublisher, request, code, eventResult, httpStatus);

    if (result.status != ShortLinkService::RedirectStatus::Success)
    {
        response->setErrorResponse(http::HttpResponse::k404NotFound,
                                   "short_link_not_found",
                                   "Short link not found");
        return;
    }

    response->setRedirect(*result.originalUrl);
}

} // namespace shortlink
