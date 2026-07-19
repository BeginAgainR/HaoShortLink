#include "shortlink/ConsumerObservabilityServer.h"

#include "shortlink/ConsumerMetrics.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace shortlink
{
namespace consumer
{
namespace
{

void sendResponse(int connection,
                  int status,
                  const char* statusText,
                  const char* contentType,
                  const std::string& body)
{
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n" +
        "Content-Type: " + contentType + "\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Connection: close\r\n\r\n" + body;
    std::size_t sent = 0;
    while (sent < response.size())
    {
        const ssize_t result = ::send(connection,
                                      response.data() + sent,
                                      response.size() - sent,
                                      MSG_NOSIGNAL);
        if (result <= 0)
        {
            return;
        }
        sent += static_cast<std::size_t>(result);
    }
}

} // namespace

ConsumerObservabilityServer::ConsumerObservabilityServer(
    Config config,
    const ConsumerMetrics* metrics)
    : config_(std::move(config))
    , metrics_(metrics)
{
    if (!config_.enabled)
    {
        return;
    }

    listenSocket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ < 0)
    {
        throw std::runtime_error("failed to create consumer observability socket");
    }
    const int reuseAddress = 1;
    ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.port));
    if (::inet_pton(AF_INET, config_.listenAddress.c_str(), &address.sin_addr) != 1)
    {
        ::close(listenSocket_);
        listenSocket_ = -1;
        throw std::invalid_argument("consumer observability address must be IPv4");
    }
    if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listenSocket_, 16) != 0)
    {
        const std::string reason = std::strerror(errno);
        ::close(listenSocket_);
        listenSocket_ = -1;
        throw std::runtime_error("failed to bind consumer observability socket: " + reason);
    }
    serverThread_ = std::thread(&ConsumerObservabilityServer::serve, this);
}

ConsumerObservabilityServer::~ConsumerObservabilityServer()
{
    stopRequested_.store(true, std::memory_order_relaxed);
    if (listenSocket_ >= 0)
    {
        ::shutdown(listenSocket_, SHUT_RDWR);
    }
    if (serverThread_.joinable())
    {
        serverThread_.join();
    }
    if (listenSocket_ >= 0)
    {
        ::close(listenSocket_);
        listenSocket_ = -1;
    }
}

void ConsumerObservabilityServer::serve()
{
    while (!stopRequested_.load(std::memory_order_relaxed))
    {
        pollfd descriptor { listenSocket_, POLLIN, 0 };
        const int pollResult = ::poll(&descriptor, 1, 500);
        if (pollResult <= 0 || (descriptor.revents & POLLIN) == 0)
        {
            continue;
        }
        const int connection = ::accept(listenSocket_, nullptr, nullptr);
        if (connection < 0)
        {
            continue;
        }
        const timeval timeout { 1, 0 };
        ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handleConnection(connection);
        ::close(connection);
    }
}

void ConsumerObservabilityServer::handleConnection(int connection) const
{
    std::array<char, 4096> buffer {};
    const ssize_t received = ::recv(connection, buffer.data(), buffer.size() - 1, 0);
    if (received <= 0)
    {
        return;
    }
    const std::string request(buffer.data(), static_cast<std::size_t>(received));
    if (request.rfind("GET /metrics ", 0) == 0)
    {
        sendResponse(connection,
                     200,
                     "OK",
                     "text/plain; version=0.0.4; charset=utf-8",
                     metrics_->renderPrometheus());
        return;
    }
    if (request.rfind("GET /health ", 0) == 0)
    {
        const bool healthy = metrics_->healthy();
        sendResponse(connection,
                     healthy ? 200 : 503,
                     healthy ? "OK" : "Service Unavailable",
                     "application/json; charset=utf-8",
                     healthy ? "{\"status\":\"up\"}" : "{\"status\":\"starting\"}");
        return;
    }
    sendResponse(connection,
                 404,
                 "Not Found",
                 "application/json; charset=utf-8",
                 "{\"error\":\"not_found\"}");
}

} // namespace consumer
} // namespace shortlink
