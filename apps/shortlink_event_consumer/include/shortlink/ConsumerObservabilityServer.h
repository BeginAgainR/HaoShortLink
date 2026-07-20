#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace shortlink
{
namespace consumer
{

class ConsumerMetrics;

class ConsumerObservabilityServer
{
public:
    struct Config
    {
        bool enabled = true;
        std::string listenAddress = "127.0.0.1";
        int port = 9091;
    };

    ConsumerObservabilityServer(Config config, const ConsumerMetrics* metrics);
    ~ConsumerObservabilityServer();

    ConsumerObservabilityServer(const ConsumerObservabilityServer&) = delete;
    ConsumerObservabilityServer& operator=(const ConsumerObservabilityServer&) = delete;

private:
    void serve();
    void handleConnection(int connection) const;

    Config config_;
    const ConsumerMetrics* metrics_;
    std::atomic<bool> stopRequested_ { false };
    int listenSocket_ { -1 };
    std::thread serverThread_;
};

} // namespace consumer
} // namespace shortlink
