#include "shortlink/AccessEvent.h"
#include "shortlink/ConsumerConfig.h"
#include "shortlink/ConsumerMetrics.h"
#include "shortlink/ConsumerObservabilityServer.h"
#include "shortlink/KafkaDeadLetterPublisher.h"
#include "shortlink/MySqlAccessStatisticsWriter.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <librdkafka/rdkafka.h>
#include <muduo/base/Logging.h>

namespace
{

volatile std::sig_atomic_t gStopRequested = 0;

void handleSignal(int)
{
    gStopRequested = 1;
}

using shortlink::consumer::AccessEventSourcePosition;
using shortlink::consumer::AccessStatisticsWriteResult;
using shortlink::consumer::ConsumerConfig;
using shortlink::consumer::ConsumerMetrics;
using shortlink::consumer::DeadLetterReason;
using shortlink::consumer::DeadLetterRecord;
using shortlink::consumer::KafkaDeadLetterPublisher;
using shortlink::consumer::MySqlAccessStatisticsWriter;
using shortlink::consumer::OrphanShortLinkEvent;

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

DeadLetterReason deadLetterReason(shortlink::AccessEventParseError error)
{
    switch (error)
    {
    case shortlink::AccessEventParseError::InvalidJson:
        return DeadLetterReason::InvalidJson;
    case shortlink::AccessEventParseError::UnsupportedSchema:
        return DeadLetterReason::UnsupportedSchema;
    case shortlink::AccessEventParseError::UnsupportedEventType:
        return DeadLetterReason::UnsupportedEventType;
    case shortlink::AccessEventParseError::InvalidContract:
    case shortlink::AccessEventParseError::None:
        return DeadLetterReason::InvalidContract;
    }
    return DeadLetterReason::InvalidContract;
}

const char* writeResultToString(AccessStatisticsWriteResult result)
{
    switch (result)
    {
    case AccessStatisticsWriteResult::Aggregated:
        return "aggregated";
    case AccessStatisticsWriteResult::Duplicate:
        return "duplicate";
    case AccessStatisticsWriteResult::IgnoredNotFound:
        return "ignored";
    }
    return "error";
}

ConsumerMetrics::MessageResult writeResultMetric(AccessStatisticsWriteResult result)
{
    switch (result)
    {
    case AccessStatisticsWriteResult::Aggregated:
        return ConsumerMetrics::MessageResult::Aggregated;
    case AccessStatisticsWriteResult::Duplicate:
        return ConsumerMetrics::MessageResult::Duplicate;
    case AccessStatisticsWriteResult::IgnoredNotFound:
        return ConsumerMetrics::MessageResult::Ignored;
    }
    return ConsumerMetrics::MessageResult::Error;
}

int nextRetryDelayMs(int delayMs, int maximumMs) noexcept
{
    return delayMs >= maximumMs - delayMs ? maximumMs : delayMs * 2;
}

class KafkaAccessEventConsumer
{
public:
    KafkaAccessEventConsumer(ConsumerConfig config, ConsumerMetrics* metrics)
        : config_(std::move(config))
        , metrics_(metrics)
    {
        MySqlAccessStatisticsWriter::Config writerConfig;
        writerConfig.host = config_.mysqlHost;
        writerConfig.user = config_.mysqlUser;
        writerConfig.password = config_.mysqlPassword;
        writerConfig.database = config_.mysqlDatabase;
        statisticsWriter_ =
            std::make_unique<MySqlAccessStatisticsWriter>(std::move(writerConfig));

        KafkaDeadLetterPublisher::Config dlqConfig;
        dlqConfig.bootstrapServers = config_.bootstrapServers;
        dlqConfig.topic = config_.dlqTopic;
        dlqConfig.clientId = config_.dlqClientId;
        dlqConfig.messageTimeoutMs = config_.dlqMessageTimeoutMs;
        dlqConfig.deliveryTimeoutMs = config_.dlqDeliveryTimeoutMs;
        deadLetterPublisher_ =
            std::make_unique<KafkaDeadLetterPublisher>(std::move(dlqConfig));

        rd_kafka_conf_t* kafkaConfig = rd_kafka_conf_new();
        if (kafkaConfig == nullptr)
        {
            throw std::runtime_error("failed to allocate Kafka consumer config");
        }

        try
        {
            setConfig(kafkaConfig, "bootstrap.servers", config_.bootstrapServers);
            setConfig(kafkaConfig, "group.id", config_.groupId);
            setConfig(kafkaConfig, "client.id", config_.clientId);
            setConfig(kafkaConfig, "enable.auto.commit", "false");
            setConfig(kafkaConfig, "enable.auto.offset.store", "false");
            setConfig(kafkaConfig, "auto.offset.reset", config_.autoOffsetReset);
            setConfig(kafkaConfig, "session.timeout.ms", std::to_string(config_.sessionTimeoutMs));
            setConfig(kafkaConfig, "enable.partition.eof", "true");
        }
        catch (...)
        {
            rd_kafka_conf_destroy(kafkaConfig);
            throw;
        }

        char error[512];
        consumer_ = rd_kafka_new(RD_KAFKA_CONSUMER, kafkaConfig, error, sizeof(error));
        if (consumer_ == nullptr)
        {
            rd_kafka_conf_destroy(kafkaConfig);
            throw std::runtime_error(std::string("failed to create Kafka consumer: ") + error);
        }

        rd_kafka_poll_set_consumer(consumer_);
        rd_kafka_topic_partition_list_t* topics = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(topics, config_.topic.c_str(), RD_KAFKA_PARTITION_UA);
        const rd_kafka_resp_err_t subscribeResult = rd_kafka_subscribe(consumer_, topics);
        rd_kafka_topic_partition_list_destroy(topics);
        if (subscribeResult != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            rd_kafka_destroy(consumer_);
            consumer_ = nullptr;
            throw std::runtime_error(
                std::string("failed to subscribe to access event topic: ") +
                rd_kafka_err2str(subscribeResult));
        }

        LOG_INFO << "event=kafka_access_event_consumer status=started"
                 << " topic=" << config_.topic
                 << " group_id=" << config_.groupId;
    }

    ~KafkaAccessEventConsumer()
    {
        if (consumer_ == nullptr)
        {
            return;
        }
        const rd_kafka_resp_err_t closeResult = rd_kafka_consumer_close(consumer_);
        if (closeResult != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            LOG_WARN << "event=kafka_access_event_consumer stage=shutdown result=failure"
                     << " reason=" << rd_kafka_err2str(closeResult);
        }
        rd_kafka_destroy(consumer_);
        consumer_ = nullptr;
    }

    KafkaAccessEventConsumer(const KafkaAccessEventConsumer&) = delete;
    KafkaAccessEventConsumer& operator=(const KafkaAccessEventConsumer&) = delete;

    void run()
    {
        while (gStopRequested == 0)
        {
            rd_kafka_message_t* message =
                rd_kafka_consumer_poll(consumer_, config_.pollTimeoutMs);
            if (message == nullptr)
            {
                refreshLag(false);
                continue;
            }
            observeMessageLag(message, false);
            try
            {
                handleMessage(message);
            }
            catch (...)
            {
                rd_kafka_message_destroy(message);
                throw;
            }
            rd_kafka_message_destroy(message);
        }
        LOG_INFO << "event=kafka_access_event_consumer status=stopping";
    }

private:
    void handleMessage(const rd_kafka_message_t* message)
    {
        if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            if (message->err != RD_KAFKA_RESP_ERR__PARTITION_EOF)
            {
                logFailure("poll", rd_kafka_message_errstr(message));
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            return;
        }

        const std::string payload(
            message->payload == nullptr ? "" : static_cast<const char*>(message->payload),
            message->len);
        const std::string key(
            message->key == nullptr ? "" : static_cast<const char*>(message->key),
            message->key_len);
        const shortlink::AccessEventParseResult parsed =
            shortlink::parseAccessEventDetailed(payload);
        if (!parsed.event)
        {
            publishToDeadLetter(message, key, payload, deadLetterReason(parsed.error));
            commitWithRetry(message);
            metrics_->recordMessage(ConsumerMetrics::MessageResult::Dlq);
            return;
        }
        if (key != parsed.event->code)
        {
            publishToDeadLetter(message, key, payload, DeadLetterReason::KeyCodeMismatch);
            commitWithRetry(message);
            metrics_->recordMessage(ConsumerMetrics::MessageResult::Dlq);
            return;
        }

        AccessStatisticsWriteResult result;
        try
        {
            result = persistWithRetry(*parsed.event, message);
        }
        catch (const OrphanShortLinkEvent&)
        {
            publishToDeadLetter(message, key, payload, DeadLetterReason::OrphanShortLink);
            commitWithRetry(message);
            metrics_->recordMessage(ConsumerMetrics::MessageResult::Dlq);
            return;
        }

        LOG_INFO << "event=kafka_access_event_consumer stage=process result="
                 << writeResultToString(result)
                 << " event_id=" << parsed.event->eventId
                 << " access_result="
                 << shortlink::accessEventResultToString(parsed.event->result)
                 << " partition=" << message->partition
                 << " offset=" << message->offset;
        commitWithRetry(message);
        metrics_->recordMessage(writeResultMetric(result));
    }

    AccessStatisticsWriteResult persistWithRetry(
        const shortlink::AccessEvent& event,
        const rd_kafka_message_t* message)
    {
        const AccessEventSourcePosition source {
            config_.topic,
            message->partition,
            message->offset
        };
        int delayMs = config_.retryInitialMs;
        for (int attempt = 1; attempt <= config_.processingMaxAttempts; ++attempt)
        {
            try
            {
                return statisticsWriter_->process(event, source);
            }
            catch (const OrphanShortLinkEvent&)
            {
                throw;
            }
            catch (const std::exception& error)
            {
                logFailure("mysql", error.what());
                if (attempt == config_.processingMaxAttempts)
                {
                    metrics_->recordMessage(ConsumerMetrics::MessageResult::Error);
                    throw;
                }
                metrics_->recordRetry(ConsumerMetrics::RetryOperation::Mysql);
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                delayMs = nextRetryDelayMs(delayMs, config_.retryMaxMs);
                try
                {
                    statisticsWriter_->reconnect();
                }
                catch (const std::exception& reconnectError)
                {
                    logFailure("mysql_reconnect", reconnectError.what());
                }
            }
        }
        throw std::runtime_error("unreachable access statistics retry state");
    }

    void publishToDeadLetter(const rd_kafka_message_t* message,
                             const std::string& key,
                             const std::string& payload,
                             DeadLetterReason reason)
    {
        const DeadLetterRecord record {
            config_.topic,
            message->partition,
            message->offset,
            key,
            payload,
            reason
        };
        int delayMs = config_.retryInitialMs;
        for (int attempt = 1; attempt <= config_.processingMaxAttempts; ++attempt)
        {
            if (deadLetterPublisher_->publish(record))
            {
                metrics_->recordDlq(ConsumerMetrics::DlqResult::Success);
                LOG_WARN << "event=kafka_access_event_consumer stage=process result=dlq"
                         << " partition=" << message->partition
                         << " offset=" << message->offset
                         << " reason=" << shortlink::consumer::deadLetterReasonToString(reason);
                return;
            }
            metrics_->recordDlq(ConsumerMetrics::DlqResult::Failure);
            logFailure("dlq_publish", shortlink::consumer::deadLetterReasonToString(reason));
            if (attempt < config_.processingMaxAttempts)
            {
                metrics_->recordRetry(ConsumerMetrics::RetryOperation::DlqPublish);
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                delayMs = nextRetryDelayMs(delayMs, config_.retryMaxMs);
            }
        }
        metrics_->recordMessage(ConsumerMetrics::MessageResult::Error);
        throw std::runtime_error("failed to publish access event to DLQ");
    }

    void commitWithRetry(const rd_kafka_message_t* message)
    {
        for (int attempt = 1; attempt <= config_.offsetCommitMaxAttempts; ++attempt)
        {
            const rd_kafka_resp_err_t result = rd_kafka_commit_message(consumer_, message, 0);
            if (result == RD_KAFKA_RESP_ERR_NO_ERROR)
            {
                metrics_->recordSuccess();
                observeMessageLag(message, true);
                return;
            }
            logFailure("commit", rd_kafka_err2str(result));
            if (attempt < config_.offsetCommitMaxAttempts)
            {
                metrics_->recordRetry(ConsumerMetrics::RetryOperation::OffsetCommit);
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
            }
        }
        metrics_->recordMessage(ConsumerMetrics::MessageResult::Error);
        throw std::runtime_error("failed to commit access event offset after configured attempts");
    }

    void observeMessageLag(const rd_kafka_message_t* message, bool committed)
    {
        if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            return;
        }
        std::int64_t low = 0;
        std::int64_t high = 0;
        const rd_kafka_resp_err_t result = rd_kafka_get_watermark_offsets(
            consumer_,
            config_.topic.c_str(),
            message->partition,
            &low,
            &high);
        if (result != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            return;
        }
        const std::int64_t nextOffset = message->offset + (committed ? 1 : 0);
        const std::int64_t minimumLag = committed ? 0 : 1;
        metrics_->setLag(message->partition,
                         std::max<std::int64_t>(minimumLag, high - nextOffset));
    }

    void refreshLag(bool force)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < nextLagRefresh_)
        {
            return;
        }
        nextLagRefresh_ = now + std::chrono::milliseconds(config_.lagRefreshMs);

        rd_kafka_topic_partition_list_t* assignment = nullptr;
        const rd_kafka_resp_err_t assignmentResult =
            rd_kafka_assignment(consumer_, &assignment);
        if (assignmentResult != RD_KAFKA_RESP_ERR_NO_ERROR || assignment == nullptr)
        {
            if (assignment != nullptr)
            {
                rd_kafka_topic_partition_list_destroy(assignment);
            }
            return;
        }
        if (assignment->cnt == 0)
        {
            rd_kafka_topic_partition_list_destroy(assignment);
            return;
        }

        const rd_kafka_resp_err_t committedResult =
            rd_kafka_committed(consumer_, assignment, config_.kafkaQueryTimeoutMs);
        if (committedResult != RD_KAFKA_RESP_ERR_NO_ERROR)
        {
            rd_kafka_topic_partition_list_destroy(assignment);
            logFailure("lag", rd_kafka_err2str(committedResult));
            return;
        }

        std::map<std::int32_t, std::int64_t> lag;
        for (int index = 0; index < assignment->cnt; ++index)
        {
            const rd_kafka_topic_partition_t& partition = assignment->elems[index];
            std::int64_t low = 0;
            std::int64_t high = 0;
            const rd_kafka_resp_err_t watermarkResult = rd_kafka_query_watermark_offsets(
                consumer_,
                partition.topic,
                partition.partition,
                &low,
                &high,
                config_.kafkaQueryTimeoutMs);
            if (watermarkResult != RD_KAFKA_RESP_ERR_NO_ERROR)
            {
                logFailure("lag", rd_kafka_err2str(watermarkResult));
                continue;
            }
            const std::int64_t committedOffset =
                partition.offset >= 0 ? partition.offset : low;
            lag[partition.partition] = std::max<std::int64_t>(0, high - committedOffset);
        }
        rd_kafka_topic_partition_list_destroy(assignment);
        metrics_->replaceLag(lag);
    }

    void logFailure(const char* stage, const char* reason)
    {
        ++failureCount_;
        if (shouldLog(failureCount_))
        {
            LOG_WARN << "event=kafka_access_event_consumer stage=" << stage
                     << " result=failure failure_count=" << failureCount_
                     << " reason=" << reason;
        }
    }

    ConsumerConfig config_;
    std::unique_ptr<MySqlAccessStatisticsWriter> statisticsWriter_;
    std::unique_ptr<KafkaDeadLetterPublisher> deadLetterPublisher_;
    ConsumerMetrics* metrics_;
    rd_kafka_t* consumer_ { nullptr };
    std::uint64_t failureCount_ { 0 };
    std::chrono::steady_clock::time_point nextLagRefresh_ {};
};

} // namespace

int main(int argc, char* argv[])
{
    const std::string configPath = argc > 1
                                       ? argv[1]
                                       : "apps/shortlink_event_consumer/config/consumer.conf.example";
    try
    {
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        ConsumerConfig config = shortlink::consumer::loadConsumerConfig(configPath);
        ConsumerMetrics metrics;
        shortlink::consumer::ConsumerObservabilityServer::Config observabilityConfig;
        observabilityConfig.enabled = config.observabilityEnabled;
        observabilityConfig.listenAddress = config.observabilityListenAddress;
        observabilityConfig.port = config.observabilityPort;
        shortlink::consumer::ConsumerObservabilityServer observability(
            std::move(observabilityConfig),
            &metrics);
        KafkaAccessEventConsumer consumer(std::move(config), &metrics);
        metrics.setHealthy(true);
        try
        {
            consumer.run();
        }
        catch (...)
        {
            metrics.setHealthy(false);
            throw;
        }
        metrics.setHealthy(false);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Access event consumer failed: " << error.what() << std::endl;
        return 1;
    }
}
