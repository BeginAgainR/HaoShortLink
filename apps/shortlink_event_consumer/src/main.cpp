#include "shortlink/AccessEvent.h"
#include "utils/Config.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
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

struct ConsumerConfig
{
    std::string bootstrapServers = "127.0.0.1:19092";
    std::string topic = "hao-shortlink.access-events.v1";
    std::string groupId = "hao-shortlink-access-event-logger-v1";
    std::string clientId = "hao-shortlink-event-consumer";
    std::string autoOffsetReset = "earliest";
    int pollTimeoutMs = 500;
    int sessionTimeoutMs = 10000;
};

ConsumerConfig loadConsumerConfig(const std::string& path)
{
    http::utils::Config file;
    if (!file.loadFromFile(path))
    {
        throw std::runtime_error("failed to load consumer config: " + path);
    }

    ConsumerConfig config;
    config.bootstrapServers = file.getString("consumer.bootstrap_servers", config.bootstrapServers);
    config.topic = file.getString("consumer.topic", config.topic);
    config.groupId = file.getString("consumer.group_id", config.groupId);
    config.clientId = file.getString("consumer.client_id", config.clientId);
    config.autoOffsetReset = file.getString("consumer.auto_offset_reset", config.autoOffsetReset);
    config.pollTimeoutMs = file.getInt("consumer.poll_timeout_ms", config.pollTimeoutMs);
    config.sessionTimeoutMs = file.getInt("consumer.session_timeout_ms", config.sessionTimeoutMs);
    if (config.bootstrapServers.empty() || config.topic.empty() || config.groupId.empty() ||
        config.clientId.empty() ||
        (config.autoOffsetReset != "earliest" && config.autoOffsetReset != "latest") ||
        config.pollTimeoutMs <= 0 || config.sessionTimeoutMs <= 0)
    {
        throw std::invalid_argument("invalid access event consumer config");
    }
    return config;
}

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

class KafkaAccessEventConsumer
{
public:
    explicit KafkaAccessEventConsumer(ConsumerConfig config)
        : config_(std::move(config))
    {
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
                continue;
            }
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
        const std::optional<shortlink::AccessEvent> event = shortlink::parseAccessEvent(payload);
        if (!event || key != event->code)
        {
            ++discardedCount_;
            if (shouldLog(discardedCount_))
            {
                LOG_WARN << "event=kafka_access_event_consumer stage=process result=discarded"
                         << " partition=" << message->partition
                         << " offset=" << message->offset
                         << " reason=" << (!event ? "invalid_event" : "key_code_mismatch")
                         << " discarded_count=" << discardedCount_;
            }
        }
        else
        {
            LOG_INFO << "event=kafka_access_event_consumer stage=process result=processed"
                     << " event_id=" << event->eventId
                     << " access_result=" << shortlink::accessEventResultToString(event->result)
                     << " partition=" << message->partition
                     << " offset=" << message->offset;
        }

        commitWithRetry(message);
    }

    void commitWithRetry(const rd_kafka_message_t* message)
    {
        for (int attempt = 1; attempt <= 3; ++attempt)
        {
            const rd_kafka_resp_err_t result = rd_kafka_commit_message(consumer_, message, 0);
            if (result == RD_KAFKA_RESP_ERR_NO_ERROR)
            {
                return;
            }
            logFailure("commit", rd_kafka_err2str(result));
            if (attempt < 3)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
            }
        }
        throw std::runtime_error("failed to commit access event offset after 3 attempts");
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
    rd_kafka_t* consumer_ { nullptr };
    std::uint64_t discardedCount_ { 0 };
    std::uint64_t failureCount_ { 0 };
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
        KafkaAccessEventConsumer consumer(loadConsumerConfig(configPath));
        consumer.run();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Access event consumer failed: " << error.what() << std::endl;
        return 1;
    }
}
