#include "shortlink/KafkaAccessEventPublisher.h"

#include "shortlink/ShortLinkMetrics.h"

#include <stdexcept>
#include <utility>

#include <muduo/base/Logging.h>

namespace shortlink
{
namespace
{

void setConfig(rd_kafka_conf_t* config, const char* key, const std::string& value)
{
    char error[512];
    if (rd_kafka_conf_set(config, key, value.c_str(), error, sizeof(error)) != RD_KAFKA_CONF_OK)
    {
        throw std::runtime_error(std::string("invalid Kafka config ") + key + ": " + error);
    }
}

bool shouldLog(std::uint64_t count)
{
    return count <= 5 || count % 1000 == 0;
}

} // namespace

KafkaAccessEventPublisher::KafkaAccessEventPublisher(Config config, ShortLinkMetrics* metrics)
    : config_(std::move(config)),
      metrics_(metrics)
{
    if (config_.bootstrapServers.empty() || config_.topic.empty() || config_.clientId.empty() ||
        config_.queueMaxMessages <= 0 || config_.messageTimeoutMs <= 0 ||
        config_.lingerMs < 0 || config_.shutdownTimeoutMs < 0)
    {
        throw std::invalid_argument("invalid Kafka access event publisher config");
    }

    rd_kafka_conf_t* kafkaConfig = rd_kafka_conf_new();
    if (kafkaConfig == nullptr)
    {
        throw std::runtime_error("failed to allocate Kafka producer config");
    }

    try
    {
        setConfig(kafkaConfig, "bootstrap.servers", config_.bootstrapServers);
        setConfig(kafkaConfig, "client.id", config_.clientId);
        setConfig(kafkaConfig, "enable.idempotence", "true");
        setConfig(kafkaConfig, "acks", "all");
        setConfig(kafkaConfig,
                  "queue.buffering.max.messages",
                  std::to_string(config_.queueMaxMessages));
        setConfig(kafkaConfig, "message.timeout.ms", std::to_string(config_.messageTimeoutMs));
        setConfig(kafkaConfig, "linger.ms", std::to_string(config_.lingerMs));
        setConfig(kafkaConfig, "compression.type", "lz4");
        rd_kafka_conf_set_opaque(kafkaConfig, this);
        rd_kafka_conf_set_dr_msg_cb(kafkaConfig, &KafkaAccessEventPublisher::deliveryCallback);
        rd_kafka_conf_set_error_cb(kafkaConfig, &KafkaAccessEventPublisher::errorCallback);
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
        throw std::runtime_error(std::string("failed to create Kafka producer: ") + error);
    }

    try
    {
        pollThread_ = std::thread(&KafkaAccessEventPublisher::pollLoop, this);
    }
    catch (...)
    {
        rd_kafka_destroy(producer_);
        producer_ = nullptr;
        throw;
    }

    LOG_INFO << "event=kafka_access_event_producer status=started topic=" << config_.topic;
}

KafkaAccessEventPublisher::~KafkaAccessEventPublisher()
{
    stopping_.store(true, std::memory_order_relaxed);
    if (pollThread_.joinable())
    {
        pollThread_.join();
    }

    if (producer_ == nullptr)
    {
        return;
    }

    const rd_kafka_resp_err_t flushResult =
        rd_kafka_flush(producer_, config_.shutdownTimeoutMs);
    if (flushResult == RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        LOG_INFO << "event=kafka_access_event_producer stage=shutdown result=flushed"
                 << " outstanding=" << rd_kafka_outq_len(producer_);
    }
    else
    {
        const int outstanding = rd_kafka_outq_len(producer_);
        LOG_WARN << "event=kafka_access_event_producer stage=shutdown result=timeout"
                 << " outstanding=" << outstanding;
        rd_kafka_purge(producer_, RD_KAFKA_PURGE_F_QUEUE | RD_KAFKA_PURGE_F_INFLIGHT);
        rd_kafka_flush(producer_, 1000);
    }
    recordQueueSize();
    rd_kafka_destroy(producer_);
    producer_ = nullptr;
}

void KafkaAccessEventPublisher::publish(const AccessEvent& event) noexcept
{
    try
    {
        const std::string payload = serializeAccessEvent(event);
        const rd_kafka_resp_err_t error = rd_kafka_producev(
            producer_,
            RD_KAFKA_V_TOPIC(config_.topic.c_str()),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_KEY(event.code.data(), event.code.size()),
            RD_KAFKA_V_VALUE(const_cast<char*>(payload.data()), payload.size()),
            RD_KAFKA_V_TIMESTAMP(event.occurredAtMs),
            RD_KAFKA_V_END);
        if (error == RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            if (metrics_ != nullptr)
            {
                metrics_->recordAccessEventEnqueue(
                    ShortLinkMetrics::AccessEventEnqueueResult::Accepted);
            }
        }
        else if (error == RD_KAFKA_RESP_ERR__QUEUE_FULL)
        {
            if (metrics_ != nullptr)
            {
                metrics_->recordAccessEventEnqueue(
                    ShortLinkMetrics::AccessEventEnqueueResult::QueueFull);
            }
            logEnqueueFailure("queue_full");
        }
        else
        {
            if (metrics_ != nullptr)
            {
                metrics_->recordAccessEventEnqueue(
                    ShortLinkMetrics::AccessEventEnqueueResult::Error);
            }
            logEnqueueFailure(rd_kafka_err2str(error));
        }
        recordQueueSize();
    }
    catch (const std::exception& error)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordAccessEventEnqueue(ShortLinkMetrics::AccessEventEnqueueResult::Error);
        }
        logEnqueueFailure(error.what());
    }
    catch (...)
    {
        if (metrics_ != nullptr)
        {
            metrics_->recordAccessEventEnqueue(ShortLinkMetrics::AccessEventEnqueueResult::Error);
        }
        logEnqueueFailure("unknown_error");
    }
}

void KafkaAccessEventPublisher::deliveryCallback(rd_kafka_t*,
                                                 const rd_kafka_message_t* message,
                                                 void* opaque)
{
    auto* publisher = static_cast<KafkaAccessEventPublisher*>(opaque);
    if (message->err == RD_KAFKA_RESP_ERR_NO_ERROR)
    {
        if (publisher->metrics_ != nullptr)
        {
            publisher->metrics_->recordAccessEventDelivery(
                ShortLinkMetrics::AccessEventDeliveryResult::Success);
        }
    }
    else
    {
        if (publisher->metrics_ != nullptr)
        {
            publisher->metrics_->recordAccessEventDelivery(
                ShortLinkMetrics::AccessEventDeliveryResult::Failure);
        }
        publisher->logDeliveryFailure(rd_kafka_err2str(message->err));
    }
    publisher->recordQueueSize();
}

void KafkaAccessEventPublisher::errorCallback(rd_kafka_t*,
                                              int error,
                                              const char* reason,
                                              void* opaque)
{
    auto* publisher = static_cast<KafkaAccessEventPublisher*>(opaque);
    publisher->logClientError(
        reason != nullptr ? reason : rd_kafka_err2str(static_cast<rd_kafka_resp_err_t>(error)));
}

void KafkaAccessEventPublisher::pollLoop() noexcept
{
    while (!stopping_.load(std::memory_order_relaxed))
    {
        rd_kafka_poll(producer_, 100);
        recordQueueSize();
    }
    rd_kafka_poll(producer_, 0);
}

void KafkaAccessEventPublisher::recordQueueSize() noexcept
{
    if (metrics_ != nullptr && producer_ != nullptr)
    {
        metrics_->setAccessEventQueueSize(static_cast<std::uint64_t>(rd_kafka_outq_len(producer_)));
    }
}

void KafkaAccessEventPublisher::logEnqueueFailure(const char* reason) noexcept
{
    const std::uint64_t count = enqueueFailureCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLog(count))
    {
        LOG_WARN << "event=kafka_access_event stage=enqueue result=failure count=" << count
                 << " reason=" << reason;
    }
}

void KafkaAccessEventPublisher::logDeliveryFailure(const char* reason) noexcept
{
    const std::uint64_t count = deliveryFailureCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLog(count))
    {
        LOG_WARN << "event=kafka_access_event stage=delivery result=failure count=" << count
                 << " reason=" << reason;
    }
}

void KafkaAccessEventPublisher::logClientError(const char* reason) noexcept
{
    const std::uint64_t count = clientErrorCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLog(count))
    {
        LOG_WARN << "event=kafka_access_event stage=client result=failure count=" << count
                 << " reason=" << reason;
    }
}

} // namespace shortlink
