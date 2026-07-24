#include "shortlink/AuthHttp.h"

#include "http/HttpRequest.h"
#include "http/HttpResponse.h"
#include "http/HttpServer.h"
#include "shortlink/AuthService.h"
#include "shortlink/SameOriginPolicy.h"
#include "shortlink/ShortLinkService.h"
#include "utils/JsonUtil.h"

#include <cctype>
#include <nlohmann/json.hpp>

namespace shortlink
{

namespace
{

bool hasJsonContentType(const http::HttpRequest& request)
{
    return request.getHeader("Content-Type").find("application/json") != std::string::npos;
}

void setSessionCookie(http::HttpResponse* response,
                      const AuthHttpConfig& config,
                      const AuthService::AuthenticatedSession& session)
{
    std::string cookie = config.cookieName + "=" + session.token +
                         "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" +
                         std::to_string(session.expiresAt - AuthService::nowEpochSeconds());
    if (config.cookieSecure)
    {
        cookie += "; Secure";
    }
    response->addHeader("Set-Cookie", cookie);
    response->addHeader("Cache-Control", "no-store");
}

void clearSessionCookie(http::HttpResponse* response, const AuthHttpConfig& config)
{
    std::string cookie = config.cookieName +
                         "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0";
    if (config.cookieSecure)
    {
        cookie += "; Secure";
    }
    response->addHeader("Set-Cookie", cookie);
    response->addHeader("Cache-Control", "no-store");
}

std::string userJson(const AuthRepository::User& user)
{
    return "{\"id\":" + std::to_string(user.id) +
           ",\"username\":\"" + http::utils::escapeJsonString(user.username) +
           "\",\"status\":\"" + userStatusToString(user.status) +
           "\",\"created_at\":\"" + ShortLinkService::formatUtcTimestamp(user.createdAt) +
           "\",\"updated_at\":\"" + ShortLinkService::formatUtcTimestamp(user.updatedAt) +
           "\"}";
}

std::optional<std::pair<std::string, std::string>> parseCredentials(
    const http::HttpRequest& request)
{
    if (!hasJsonContentType(request) || request.getBody().size() > 4096)
    {
        return std::nullopt;
    }
    const nlohmann::json object = nlohmann::json::parse(
        request.getBody(), nullptr, false, true);
    if (object.is_discarded() || !object.is_object() || object.size() != 2 ||
        !object.contains("username") || !object["username"].is_string() ||
        !object.contains("password") || !object["password"].is_string())
    {
        return std::nullopt;
    }
    return std::make_pair(object["username"].get<std::string>(),
                          object["password"].get<std::string>());
}

void respondWithSession(http::HttpResponse* response,
                        const AuthHttpConfig& config,
                        const AuthService::AuthenticatedSession& session,
                        http::HttpResponse::HttpStatusCode status,
                        const std::string& statusMessage)
{
    setSessionCookie(response, config, session);
    response->setStatusCode(status);
    response->setStatusMessage(statusMessage);
    response->setJsonBody("{\"user\":" + userJson(session.user) +
                          ",\"session_expires_at\":\"" +
                          ShortLinkService::formatUtcTimestamp(session.expiresAt) + "\"}");
}

void handleRegister(const http::HttpRequest& request,
                    http::HttpResponse* response,
                    AuthService* service,
                    const AuthHttpConfig& config)
{
    response->addHeader("Cache-Control", "no-store");
    if (!requireSameOriginRequest(request, response))
    {
        return;
    }
    if (!config.registrationEnabled)
    {
        response->setErrorResponse(http::HttpResponse::k403Forbidden,
                                   "registration_disabled",
                                   "Registration is disabled");
        return;
    }
    const auto credentials = parseCredentials(request);
    if (!credentials)
    {
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_request",
                                   "Request must contain username and password");
        return;
    }
    const AuthService::Result result = service->registerUser(credentials->first,
                                                             credentials->second);
    switch (result.status)
    {
    case AuthService::ResultStatus::Success:
        respondWithSession(response,
                           config,
                           *result.session,
                           http::HttpResponse::k201Created,
                           "Created");
        return;
    case AuthService::ResultStatus::InvalidUsername:
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_username",
                                   "Username must be 3-32 ASCII letters, numbers, _ or -");
        return;
    case AuthService::ResultStatus::InvalidPassword:
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_password",
                                   "Password must be between 10 and 128 bytes");
        return;
    case AuthService::ResultStatus::UsernameConflict:
        response->setErrorResponse(http::HttpResponse::k409Conflict,
                                   "username_conflict",
                                   "Username already exists");
        return;
    default:
        response->setErrorResponse(http::HttpResponse::k500InternalServerError,
                                   "authentication_error",
                                   "Unable to create account");
    }
}

void handleLogin(const http::HttpRequest& request,
                 http::HttpResponse* response,
                 AuthService* service,
                 const AuthHttpConfig& config)
{
    response->addHeader("Cache-Control", "no-store");
    if (!requireSameOriginRequest(request, response))
    {
        return;
    }
    const auto credentials = parseCredentials(request);
    if (!credentials)
    {
        response->setErrorResponse(http::HttpResponse::k400BadRequest,
                                   "invalid_request",
                                   "Request must contain username and password");
        return;
    }
    const AuthService::Result result = service->login(credentials->first, credentials->second);
    if (result.status == AuthService::ResultStatus::Success && result.session)
    {
        respondWithSession(response,
                           config,
                           *result.session,
                           http::HttpResponse::k200Ok,
                           "OK");
        return;
    }
    if (result.status == AuthService::ResultStatus::InvalidCredentials)
    {
        response->setErrorResponse(http::HttpResponse::k401Unauthorized,
                                   "invalid_credentials",
                                   "Invalid username or password");
        return;
    }
    response->setErrorResponse(http::HttpResponse::k500InternalServerError,
                               "authentication_error",
                               "Unable to create session");
}

} // namespace

std::string sessionTokenFromRequest(const http::HttpRequest& request,
                                    const AuthHttpConfig& config)
{
    const std::string cookie = request.getHeader("Cookie");
    std::size_t begin = 0;
    while (begin < cookie.size())
    {
        while (begin < cookie.size() && (cookie[begin] == ';' ||
                                         std::isspace(static_cast<unsigned char>(cookie[begin]))))
        {
            ++begin;
        }
        const std::size_t end = cookie.find(';', begin);
        const std::string item = cookie.substr(begin,
                                               end == std::string::npos
                                                   ? std::string::npos
                                                   : end - begin);
        const std::size_t equals = item.find('=');
        if (equals != std::string::npos && item.substr(0, equals) == config.cookieName)
        {
            return item.substr(equals + 1);
        }
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return {};
}

std::optional<AuthRepository::User> authenticateRequest(
    const http::HttpRequest& request,
    const AuthService& service,
    const AuthHttpConfig& config)
{
    const std::string token = sessionTokenFromRequest(request, config);
    return token.empty() ? std::nullopt : service.authenticate(token);
}

bool requireAuthenticatedUser(const http::HttpRequest& request,
                              http::HttpResponse* response,
                              const AuthService& service,
                              const AuthHttpConfig& config,
                              AuthRepository::User* user)
{
    const auto authenticated = authenticateRequest(request, service, config);
    if (!authenticated)
    {
        response->addHeader("Cache-Control", "no-store");
        response->setErrorResponse(http::HttpResponse::k401Unauthorized,
                                   "authentication_required",
                                   "Authentication required");
        return false;
    }
    *user = *authenticated;
    return true;
}

void registerAuthRoutes(http::HttpServer* server,
                        AuthService* service,
                        AuthHttpConfig config)
{
    server->Post("/api/auth/register", [service, config](const http::HttpRequest& request,
                                                          http::HttpResponse* response) {
        handleRegister(request, response, service, config);
    });
    server->Post("/api/auth/login", [service, config](const http::HttpRequest& request,
                                                       http::HttpResponse* response) {
        handleLogin(request, response, service, config);
    });
    server->addRoute(http::HttpRequest::kDelete,
                     "/api/auth/session",
                     [service, config](const http::HttpRequest& request,
                                       http::HttpResponse* response) {
                         response->addHeader("Cache-Control", "no-store");
                         if (!requireSameOriginRequest(request, response))
                         {
                             return;
                         }
                         const std::string token = sessionTokenFromRequest(request, config);
                         service->logout(token);
                         clearSessionCookie(response, config);
                         response->setStatusCode(http::HttpResponse::k204NoContent);
                         response->setStatusMessage("No Content");
                         response->setContentLength(0);
                         response->setBody("");
                     });
    server->Get("/api/me", [service, config](const http::HttpRequest& request,
                                              http::HttpResponse* response) {
        AuthRepository::User user;
        if (!requireAuthenticatedUser(request, response, *service, config, &user))
        {
            return;
        }
        response->addHeader("Cache-Control", "no-store");
        response->setStatusCode(http::HttpResponse::k200Ok);
        response->setStatusMessage("OK");
        response->setJsonBody(userJson(user));
    });
}

} // namespace shortlink
