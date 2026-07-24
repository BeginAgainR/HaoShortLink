#pragma once

#include "shortlink/AuthRepository.h"

#include <optional>
#include <string>

namespace http
{
class HttpRequest;
class HttpResponse;
class HttpServer;
}

namespace shortlink
{

class AuthService;

struct AuthHttpConfig
{
    bool registrationEnabled { true };
    bool cookieSecure { false };
    std::string cookieName { "hao_session" };
};

std::string sessionTokenFromRequest(const http::HttpRequest& request,
                                    const AuthHttpConfig& config);
std::optional<AuthRepository::User> authenticateRequest(
    const http::HttpRequest& request,
    const AuthService& service,
    const AuthHttpConfig& config);
bool requireAuthenticatedUser(const http::HttpRequest& request,
                              http::HttpResponse* response,
                              const AuthService& service,
                              const AuthHttpConfig& config,
                              AuthRepository::User* user);
void registerAuthRoutes(http::HttpServer* server,
                        AuthService* service,
                        AuthHttpConfig config);

} // namespace shortlink
