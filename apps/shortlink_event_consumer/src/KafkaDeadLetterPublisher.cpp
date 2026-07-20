#include "shortlink/KafkaDeadLetterPublisher.h"

#include "shortlink/AccessEvent.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace shortlink
{
namespace consumer
{

namespace
{

constexpr std::size_t kMaxEncodedSourceBytes = 64 * 1024;

void setConfig(rd_kafka_conf_t* config, const char* key, const std::string& value)
{
    char error[512];
    if (rd_kafka_conf_set(config, key, value.c_str(), error, sizeof(error)) != RD_KAFKA_CONF_OK)
    {
        throw std::runtime_error(std::string("invalid Kafka DLQ config ") + key + ": " + error);
    }
}

std::string base64EncodePrefix(const std::string& input, std::size_t maxBytes)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::size_t size = std::min(input.size(), maxBytes);
    std::string output;
    output.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        const std::uint32_t first = static_cast<unsigned char>(input[i]);
        const std::uint32_t second = i + 1 < size
                                         ? static_cast<unsigned char>(input[i + 1])
                                         : 0;
        const std::uint32_t third = i + 2 < size
                                        ? static_cast<unsigned char>(input[i + 2])
                                        : 0;
        const std::uint32_t value = (first << 16) | (second << 8) | third;
        output.push_back(kAlphabet[(value >> 18) & 0x3f]);
        output.push_back(kAlphabet[(value >> 12) & 0x3f]);
        output.push_back(i + 1 < size ? kAlphabet[(value >> 6) & 0x3f] : '=');
        output.push_back(i + 2 < size ? kAlphabet[value & 0x3f] : '=');
    }
    return output;
}

} // namespace

const char* deadLetterReasonToString(DeadLetterReason reason) noexcept
{
    switch (reason)
    {
    case DeadLetterReason::InvalidJson:
        return "invalid_json";
    case DeadLetterReason::UnsupportedSchema:
        return "unsupported_schema";
    case DeadLetterReason::UnsupportedEventType:
        return "unsupported_event_type";
    case DeadLetterReason::InvalidContract:
        return "invalid_contract";
    case DeadLetterReason::KeyCodeMismatch:
        return "key_code_mismatch";
    case DeadLetterReason::OrphanShortLink:
        return "orphan_short_link";
    }
    return "invalid_contract";
}

std::string serializeDeadLetterRecord(const DeadLetterRecord& record,
                                      std::int64_t failedAtMs)
{
    if (record.sourceTopic.empty() || record.sourcePartition < 0 ||
        record.sourceOffset < 0 || failedAtMs <= 0)
    {
        throw std::invalid_argument("invalid dead-letter source record");
    }

    nlohmann::ordered_json payload;
    payload["schema_version"] = 1;
    payload["event_type"] = "short_link_access_dead_letter";
    payload["failed_at_ms"] = failedAtMs;
    payload["reason"] = deadLetterReasonToString(record.reason);
    payload["source_topic"] = record.sourceTopic;
    payload["source_partition"] = record.sourcePartition;
    payload["source_offset"] = record.sourceOffset;
    payload["original_key_base64"] = base64EncodePrefix(record.key, kMaxEncodedSourceBytes);
    payload["original_key_size"] = record.key.size();
    payload["original_key_truncated"] = record.key.size() > kMaxEncodedSourceBytes;
    payload["original_payload_base64"] =
        base64EncodePrefix(record.payload, kMaxEncodedSourceBytes);
    payload["original_payload_size"] = record.payload.size();
    payload["original_payload_truncated"] =
        record.payload.size() > kMaxEncodedSourceBytes;
    return payload.dump();
}

KafkaDeadLetterPublisher::KafkaDeadLetterPublisher(Config config)
    : config_(std::move(config))
{
    if (config_.bootstrapServers.empty() || config_.topic.empty() || config_.clientId.empty() ||
        config_.messageTimeoutMs <= 0 || config_.deliveryTimeoutMs <= 0)
    {
        throw std::invalid_argument("invalid Kafka DLQ publisher config");
    }

    rd_kafka_conf_t* kafkaConfig = rd_kafka_conf_new();
    if (kafkaConfig == nullptr)
    {
        throw std::runtime_error("failed to allocate Kafka DLQ producer config");
    }
    try
    {
        setConfig(kafkaConfig, "bootstrap.servers", config_.bootstrapServers);
        setConfig(kafkaConfig, "client.id", config_.clientId);
        setConfig(kafkaConfig, "enable.idempotence", "true");
        setConfig(kafkaConfig, "acks", "all");
        setConfig(kafkaConfig, "message.timeout.ms", std::to_string(config_.messageTimeoutMs));
        rd_kafka_conf_set_opaque(kafkaConfig, this);
        rd_kafka_conf_set_dr_msg_cb(kafkaConfig, &KafkaDeadLetterPublisher::deliveryCallback);
    }
    catch (...)
    {
        rd_kafka_conf_destroy(kafkaConfig);
        throw;
    }

    char error[512];
    producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, kafkaConfig, error, sizeof(error));
    if (producer_ == nullptr)
    {
        rd_kafka_conf_destroy(kafkaConfig);
        throw std::runtime_error(std::string("failed to create Kafka DLQ producer: ") + error);
    }
}

KafkaDeadLetterPublisher::~KafkaDeadLetterPublisher()
{
    if (producer_ != nullptr)
    {
        rd_kafka_flush(producer_, config_.deliveryTimeoutMs);
        rd_kafka_destroy(producer_);
        producer_ = nullptr;
    }
}

bool KafkaDeadLetterPublisher::publish(const DeadLetterRecord& record) noexcept
{
    try
    {
        const std::string payload = serializeDeadLetterRecord(record, nowEpochMilliseconds());
        deliveryObserved_.store(false, std::memory_order_relaxed);
        deliveryError_.store(static_cast<int>(RD_KAFKA_RESP_ERR_NO_ERROR),
                             std::memory_order_relaxed);

        const rd_kafka_resp_err_t result = rd_kafka_producev(
            producer_,
            RD_KAFKA_V_TOPIC(config_.topic.c_str()),
            RD_KAFKA_V_KEY(record.key.data(), record.key.size()),
            RD_KAFKA_V_VALUE(const_cast<char*>(payload.data()), payload.size()),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_END);
        if (result != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            return false;
        }

        if (rd_kafka_flush(producer_, config_.deliveryTimeoutMs) != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            return false;
        }
        return deliveryObserved_.load(std::memory_order_relaxed) &&
               deliveryError_.load(std::memory_order_relaxed) ==
                   static_cast<int>(RD_KAFKA_RESP_ERR_NO_ERROR);
    }
    catch (...)
    {
        return false;
    }
}

void KafkaDeadLetterPublisher::deliveryCallback(rd_kafka_t*,
                                                const rd_kafka_message_t* message,
                                                void* opaque) noexcept
{
    auto* publisher = static_cast<KafkaDeadLetterPublisher*>(opaque);
    if (publisher == nullptr || message == nullptr)
    {
        return;
    }
    publisher->deliveryError_.store(static_cast<int>(message->err),
                                    std::memory_order_relaxed);
    publisher->deliveryObserved_.store(true, std::memory_order_relaxed);
}

} // namespace consumer
} // namespace shortlink
