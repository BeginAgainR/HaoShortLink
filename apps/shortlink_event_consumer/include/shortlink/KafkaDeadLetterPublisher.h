#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <librdkafka/rdkafka.h>

namespace shortlink
{
namespace consumer
{

enum class DeadLetterReason
{
    InvalidJson,
    UnsupportedSchema,
    UnsupportedEventType,
    InvalidContract,
    KeyCodeMismatch,
    OrphanShortLink
};

const char* deadLetterReasonToString(DeadLetterReason reason) noexcept;

struct DeadLetterRecord
{
    std::string sourceTopic;
    std::int32_t sourcePartition { -1 };
    std::int64_t sourceOffset { -1 };
    std::string key;
    std::string payload;
    DeadLetterReason reason { DeadLetterReason::InvalidContract };
};

std::string serializeDeadLetterRecord(const DeadLetterRecord& record,
                                      std::int64_t failedAtMs);

class KafkaDeadLetterPublisher
{
public:
    struct Config
    {
        std::string bootstrapServers;
        std::string topic;
        std::string clientId;
        int messageTimeoutMs { 10000 };
        int deliveryTimeoutMs { 12000 };
    };

    explicit KafkaDeadLetterPublisher(Config config);
    ~KafkaDeadLetterPublisher();

    KafkaDeadLetterPublisher(const KafkaDeadLetterPublisher&) = delete;
    KafkaDeadLetterPublisher& operator=(const KafkaDeadLetterPublisher&) = delete;

    bool publish(const DeadLetterRecord& record) noexcept;

private:
    static void deliveryCallback(rd_kafka_t*,
                                 const rd_kafka_message_t* message,
                                 void* opaque) noexcept;

    Config config_;
    rd_kafka_t* producer_ { nullptr };
    std::atomic<bool> deliveryObserved_ { false };
    std::atomic<int> deliveryError_ { static_cast<int>(RD_KAFKA_RESP_ERR_NO_ERROR) };
};

} // namespace consumer
} // namespace shortlink
