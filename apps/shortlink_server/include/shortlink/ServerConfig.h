#pragma once

#include <string>

namespace shortlink
{

struct ServerConfig
{
    std::string name { "HaoShortLink" };
    int port { 8080 };
    int threadNum { 4 };
    bool metricsEnabled { true };
    std::string storageType { "memory" };
    bool statisticsEnabled { false };
    std::string mysqlHost { "tcp://127.0.0.1:3306" };
    std::string mysqlUser { "root" };
    std::string mysqlPassword;
    std::string mysqlDatabase { "hao_shortlink" };
    int mysqlPoolSize { 4 };
    bool redisEnabled { false };
    std::string redisHost { "127.0.0.1" };
    int redisPort { 6379 };
    int redisDatabase { 0 };
    int redisTtlSeconds { 3600 };
    std::string redisKeyPrefix { "shortlink:" };
    bool rateLimitEnabled { false };
    int rateLimitRequests { 100 };
    int rateLimitWindowSeconds { 60 };
    std::string rateLimitKeyPrefix { "rate-limit:create:" };
    bool kafkaEnabled { false };
    std::string kafkaBootstrapServers { "127.0.0.1:9092" };
    std::string kafkaTopic { "hao-shortlink.access-events.v1" };
    std::string kafkaClientId { "hao-shortlink-server" };
    int kafkaQueueMaxMessages { 10000 };
    int kafkaMessageTimeoutMs { 10000 };
    int kafkaLingerMs { 5 };
    int kafkaShutdownTimeoutMs { 3000 };
    bool registrationEnabled { true };
    bool authCookieSecure { false };
    int sessionTtlSeconds { 604800 };
};

ServerConfig loadServerConfig(const std::string& configPath);

} // namespace shortlink
