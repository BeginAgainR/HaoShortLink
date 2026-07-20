#pragma once

#include <string>

namespace shortlink
{
namespace consumer
{

struct ConsumerConfig
{
    std::string bootstrapServers = "127.0.0.1:19092";
    std::string topic = "hao-shortlink.access-events.v1";
    std::string groupId = "hao-shortlink-access-statistics-v1";
    std::string clientId = "hao-shortlink-access-statistics-consumer";
    std::string autoOffsetReset = "earliest";
    int pollTimeoutMs = 500;
    int sessionTimeoutMs = 10000;

    std::string dlqTopic = "hao-shortlink.access-events.dlq.v1";
    std::string dlqClientId = "hao-shortlink-access-statistics-dlq";
    int dlqMessageTimeoutMs = 10000;
    int dlqDeliveryTimeoutMs = 12000;

    std::string mysqlHost = "tcp://127.0.0.1:3306";
    std::string mysqlUser = "root";
    std::string mysqlPassword;
    std::string mysqlDatabase = "hao_shortlink";

    int processingMaxAttempts = 5;
    int retryInitialMs = 100;
    int retryMaxMs = 5000;
    int offsetCommitMaxAttempts = 3;

    bool observabilityEnabled = true;
    std::string observabilityListenAddress = "127.0.0.1";
    int observabilityPort = 9091;
    int lagRefreshMs = 5000;
    int kafkaQueryTimeoutMs = 1000;
};

ConsumerConfig loadConsumerConfig(const std::string& path);

} // namespace consumer
} // namespace shortlink
