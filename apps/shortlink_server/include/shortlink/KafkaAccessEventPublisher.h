#pragma once

#include "shortlink/AccessEventPublisher.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <librdkafka/rdkafka.h>

namespace shortlink
{

class ShortLinkMetrics;

class KafkaAccessEventPublisher final : public AccessEventPublisher
{
public:
    struct Config
    {
        std::string bootstrapServers = "127.0.0.1:9092";
        std::string topic = "hao-shortlink.access-events.v1";
        std::string clientId = "hao-shortlink-server";
        int queueMaxMessages = 10000;
        int messageTimeoutMs = 10000;
        int lingerMs = 5;
        int shutdownTimeoutMs = 3000;
    };

    explicit KafkaAccessEventPublisher(Config config, ShortLinkMetrics* metrics = nullptr);
    ~KafkaAccessEventPublisher() override;

    KafkaAccessEventPublisher(const KafkaAccessEventPublisher&) = delete;
    KafkaAccessEventPublisher& operator=(const KafkaAccessEventPublisher&) = delete;

    void publish(const AccessEvent& event) noexcept override;

private:
    static void deliveryCallback(rd_kafka_t* producer,
                                 const rd_kafka_message_t* message,
                                 void* opaque);
    static void errorCallback(rd_kafka_t* producer,
                              int error,
                              const char* reason,
                              void* opaque);
    void pollLoop() noexcept;
    void recordQueueSize() noexcept;
    void logEnqueueFailure(const char* reason) noexcept;
    void logDeliveryFailure(const char* reason) noexcept;
    void logClientError(const char* reason) noexcept;

    Config config_;
    ShortLinkMetrics* metrics_;
    rd_kafka_t* producer_ { nullptr };
    std::atomic<bool> stopping_ { false };
    std::atomic<std::uint64_t> enqueueFailureCount_ { 0 };
    std::atomic<std::uint64_t> deliveryFailureCount_ { 0 };
    std::atomic<std::uint64_t> clientErrorCount_ { 0 };
    std::thread pollThread_;
};

} // namespace shortlink
