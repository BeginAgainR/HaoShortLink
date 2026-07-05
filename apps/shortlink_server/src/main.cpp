#include "http/HttpServer.h"
#include "shortlink/MemoryShortLinkRepository.h"
#include "shortlink/MySqlShortLinkRepository.h"
#include "shortlink/ShortLinkService.h"
#include "utils/Config.h"
#include "utils/JsonUtil.h"
#include "utils/db/DbConnectionPool.h"

#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <muduo/base/Logging.h>

namespace
{

struct ServerConfig
{
    std::string name = "HaoShortLink";
    int port = 8080;
    int threadNum = 4;
    std::string storageType = "memory";
    std::string mysqlHost = "tcp://127.0.0.1:3306";
    std::string mysqlUser = "root";
    std::string mysqlPassword;
    std::string mysqlDatabase = "hao_shortlink";
    int mysqlPoolSize = 4;
};

ServerConfig loadServerConfig(const std::string& configPath)
{
    ServerConfig serverConfig;

    http::utils::Config config;
    if (!configPath.empty())
    {
        if (!config.loadFromFile(configPath))
        {
            std::cerr << "Failed to load config file: " << configPath
                      << ". Using default server config." << std::endl;
            return serverConfig;
        }

        serverConfig.name = config.getString("server.name", serverConfig.name);
        serverConfig.port = config.getInt("server.port", serverConfig.port);
        serverConfig.threadNum = config.getInt("server.thread_num", serverConfig.threadNum);
        serverConfig.storageType = config.getString("storage.type", serverConfig.storageType);
        serverConfig.mysqlHost = config.getString("mysql.host", serverConfig.mysqlHost);
        serverConfig.mysqlUser = config.getString("mysql.user", serverConfig.mysqlUser);
        serverConfig.mysqlPassword = config.getString("mysql.password", serverConfig.mysqlPassword);
        serverConfig.mysqlDatabase = config.getString("mysql.database", serverConfig.mysqlDatabase);
        serverConfig.mysqlPoolSize = config.getInt("mysql.pool_size", serverConfig.mysqlPoolSize);
    }

    if (serverConfig.port <= 0 || serverConfig.port > 65535)
    {
        std::cerr << "Invalid server.port. Using default port 8080." << std::endl;
        serverConfig.port = 8080;
    }

    if (serverConfig.threadNum <= 0)
    {
        std::cerr << "Invalid server.thread_num. Using default thread_num 4." << std::endl;
        serverConfig.threadNum = 4;
    }

    if (serverConfig.storageType != "memory" && serverConfig.storageType != "mysql")
    {
        std::cerr << "Invalid storage.type. Using memory storage." << std::endl;
        serverConfig.storageType = "memory";
    }

    if (serverConfig.mysqlPoolSize <= 0)
    {
        std::cerr << "Invalid mysql.pool_size. Using default pool_size 4." << std::endl;
        serverConfig.mysqlPoolSize = 4;
    }

    return serverConfig;
}

void setJsonResponse(http::HttpResponse* resp,
                     http::HttpResponse::HttpStatusCode statusCode,
                     const std::string& statusMessage,
                     const std::string& body)
{
    resp->setStatusCode(statusCode);
    resp->setStatusMessage(statusMessage);
    resp->setJsonBody(body);
}

bool hasJsonContentType(const http::HttpRequest& req)
{
    const std::string contentType = req.getHeader("Content-Type");
    return contentType.find("application/json") != std::string::npos;
}

void skipWhitespace(const std::string& value, std::size_t* pos)
{
    while (*pos < value.size() && std::isspace(static_cast<unsigned char>(value[*pos])))
    {
        ++(*pos);
    }
}

bool consumeChar(const std::string& value, std::size_t* pos, char expected)
{
    skipWhitespace(value, pos);
    if (*pos >= value.size() || value[*pos] != expected)
    {
        return false;
    }

    ++(*pos);
    return true;
}

std::optional<std::string> parseJsonString(const std::string& value, std::size_t* pos)
{
    skipWhitespace(value, pos);
    if (*pos >= value.size() || value[*pos] != '"')
    {
        return std::nullopt;
    }

    ++(*pos);

    std::string parsed;
    while (*pos < value.size())
    {
        const char current = value[(*pos)++];
        if (current == '"')
        {
            return parsed;
        }

        if (current == '\\')
        {
            if (*pos >= value.size())
            {
                return std::nullopt;
            }

            const char escaped = value[(*pos)++];
            switch (escaped)
            {
            case '"':
            case '\\':
            case '/':
                parsed.push_back(escaped);
                break;
            case 'n':
                parsed.push_back('\n');
                break;
            case 'r':
                parsed.push_back('\r');
                break;
            case 't':
                parsed.push_back('\t');
                break;
            default:
                return std::nullopt;
            }
            continue;
        }

        parsed.push_back(current);
    }

    return std::nullopt;
}

std::optional<std::string> parseUrlFromCreateRequest(const std::string& body)
{
    std::size_t pos = 0;
    if (!consumeChar(body, &pos, '{'))
    {
        return std::nullopt;
    }

    bool foundUrl = false;
    std::optional<std::string> url;

    while (true)
    {
        skipWhitespace(body, &pos);
        if (pos < body.size() && body[pos] == '}')
        {
            ++pos;
            break;
        }

        const std::optional<std::string> key = parseJsonString(body, &pos);
        if (!key || !consumeChar(body, &pos, ':'))
        {
            return std::nullopt;
        }

        const std::optional<std::string> value = parseJsonString(body, &pos);
        if (!value)
        {
            return std::nullopt;
        }

        if (*key == "url")
        {
            foundUrl = true;
            url = *value;
        }

        skipWhitespace(body, &pos);
        if (pos < body.size() && body[pos] == ',')
        {
            ++pos;
            continue;
        }
        if (pos < body.size() && body[pos] == '}')
        {
            ++pos;
            break;
        }

        return std::nullopt;
    }

    skipWhitespace(body, &pos);
    if (pos != body.size() || !foundUrl)
    {
        return std::nullopt;
    }

    return url;
}

void handleHealth(const http::HttpRequest& req, http::HttpResponse* resp)
{
    (void)req;
    resp->setStatusCode(http::HttpResponse::k200Ok);
    resp->setStatusMessage("OK");
    resp->setJsonBody("{\"status\":\"ok\"}");
}

void handleCreateShortLink(const http::HttpRequest& req,
                           http::HttpResponse* resp,
                           shortlink::ShortLinkService* service)
{
    if (!hasJsonContentType(req))
    {
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_request",
                               "Content-Type must be application/json");
        return;
    }

    const std::optional<std::string> originalUrl = parseUrlFromCreateRequest(req.getBody());
    if (!originalUrl)
    {
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_request",
                               "Request body must contain a string url field");
        return;
    }

    const std::optional<shortlink::ShortLinkService::ShortLink> shortLink =
        service->createShortLink(*originalUrl);
    if (!shortLink)
    {
        resp->setErrorResponse(http::HttpResponse::k400BadRequest,
                               "invalid_url",
                               "URL must start with http:// or https://");
        return;
    }

    const std::string body =
        "{\"code\":\"" + http::utils::escapeJsonString(shortLink->code) +
        "\",\"short_url\":\"" + http::utils::escapeJsonString(shortLink->shortUrl) +
        "\",\"original_url\":\"" + http::utils::escapeJsonString(shortLink->originalUrl) +
        "\"}";

    setJsonResponse(resp,
                    http::HttpResponse::k201Created,
                    "Created",
                    body);
}

void handleRedirect(const http::HttpRequest& req,
                    http::HttpResponse* resp,
                    shortlink::ShortLinkService* service)
{
    const std::string code = req.getPathParameters("param1");
    const std::optional<std::string> originalUrl = service->findOriginalUrl(code);
    if (!originalUrl)
    {
        resp->setErrorResponse(http::HttpResponse::k404NotFound,
                               "short_link_not_found",
                               "Short link not found");
        return;
    }

    resp->setRedirect(*originalUrl);
}

} // namespace

int main(int argc, char* argv[])
{
    const std::string configPath = argc > 1 ? argv[1] : "apps/shortlink_server/config/server.conf.example";
    const ServerConfig config = loadServerConfig(configPath);

    LOG_INFO << "Starting " << config.name << " on port " << config.port
             << " with " << config.threadNum << " worker threads";

    http::HttpServer server(config.port, config.name);
    std::unique_ptr<shortlink::ShortLinkRepository> shortLinkRepository;
    if (config.storageType == "mysql")
    {
        http::db::DbConnectionPool::getInstance().init(config.mysqlHost,
                                                       config.mysqlUser,
                                                       config.mysqlPassword,
                                                       config.mysqlDatabase,
                                                       static_cast<std::size_t>(config.mysqlPoolSize));
        shortLinkRepository = std::make_unique<shortlink::MySqlShortLinkRepository>();
    }
    else
    {
        shortLinkRepository = std::make_unique<shortlink::MemoryShortLinkRepository>();
    }

    shortlink::ShortLinkService shortLinkService(*shortLinkRepository);

    server.setThreadNum(config.threadNum);
    server.Get("/api/health", handleHealth);
    server.Post("/api/short-links", [&shortLinkService](const http::HttpRequest& req, http::HttpResponse* resp) {
        handleCreateShortLink(req, resp, &shortLinkService);
    });
    server.addRoute(http::HttpRequest::kGet,
                    "/s/:code",
                    [&shortLinkService](const http::HttpRequest& req, http::HttpResponse* resp) {
                        handleRedirect(req, resp, &shortLinkService);
                    });
    server.start();
}
