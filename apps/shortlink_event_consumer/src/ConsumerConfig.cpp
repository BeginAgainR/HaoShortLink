#include "shortlink/ConsumerConfig.h"

#include "utils/Config.h"

#include <stdexcept>

namespace shortlink
{
namespace consumer
{

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

    config.dlqTopic = file.getString("consumer.dlq_topic", config.dlqTopic);
    config.dlqClientId = file.getString("consumer.dlq_client_id", config.dlqClientId);
    config.dlqMessageTimeoutMs =
        file.getInt("consumer.dlq_message_timeout_ms", config.dlqMessageTimeoutMs);
    config.dlqDeliveryTimeoutMs =
        file.getInt("consumer.dlq_delivery_timeout_ms", config.dlqDeliveryTimeoutMs);

    config.mysqlHost = file.getString("mysql.host", config.mysqlHost);
    config.mysqlUser = file.getString("mysql.user", config.mysqlUser);
    config.mysqlPassword = file.getString("mysql.password", config.mysqlPassword);
    config.mysqlDatabase = file.getString("mysql.database", config.mysqlDatabase);

    config.processingMaxAttempts =
        file.getInt("consumer.processing_max_attempts", config.processingMaxAttempts);
    config.retryInitialMs = file.getInt("consumer.retry_initial_ms", config.retryInitialMs);
    config.retryMaxMs = file.getInt("consumer.retry_max_ms", config.retryMaxMs);
    config.offsetCommitMaxAttempts =
        file.getInt("consumer.offset_commit_max_attempts", config.offsetCommitMaxAttempts);
    config.observabilityEnabled =
        file.getBool("consumer.observability_enabled", config.observabilityEnabled);
    config.observabilityListenAddress = file.getString(
        "consumer.observability_listen_address",
        config.observabilityListenAddress);
    config.observabilityPort =
        file.getInt("consumer.observability_port", config.observabilityPort);
    config.lagRefreshMs = file.getInt("consumer.lag_refresh_ms", config.lagRefreshMs);
    config.kafkaQueryTimeoutMs =
        file.getInt("consumer.kafka_query_timeout_ms", config.kafkaQueryTimeoutMs);

    if (config.bootstrapServers.empty() || config.topic.empty() || config.groupId.empty() ||
        config.clientId.empty() || config.dlqTopic.empty() || config.dlqClientId.empty() ||
        config.mysqlHost.empty() || config.mysqlUser.empty() || config.mysqlDatabase.empty() ||
        (config.autoOffsetReset != "earliest" && config.autoOffsetReset != "latest") ||
        config.pollTimeoutMs <= 0 || config.sessionTimeoutMs <= 0 ||
        config.dlqMessageTimeoutMs <= 0 || config.dlqDeliveryTimeoutMs <= 0 ||
        config.processingMaxAttempts <= 0 || config.retryInitialMs <= 0 ||
        config.retryMaxMs < config.retryInitialMs || config.offsetCommitMaxAttempts <= 0 ||
        config.observabilityListenAddress.empty() || config.observabilityPort <= 0 ||
        config.observabilityPort > 65535 || config.lagRefreshMs <= 0 ||
        config.kafkaQueryTimeoutMs <= 0)
    {
        throw std::invalid_argument("invalid access event consumer config");
    }
    return config;
}

} // namespace consumer
} // namespace shortlink
