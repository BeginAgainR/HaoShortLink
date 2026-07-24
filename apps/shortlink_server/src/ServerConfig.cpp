#include "shortlink/ServerConfig.h"

#include "utils/Config.h"

#include <iostream>

namespace shortlink
{

ServerConfig loadServerConfig(const std::string& configPath)
{
    ServerConfig serverConfig;

    http::utils::Config config;
    if (!configPath.empty())
    {
        if (!config.loadFromFile(configPath))
        {
            std::cerr << "Failed to load config file: " << configPath
                      << ". Using default server config." << std::endl;
            return serverConfig;
        }

        serverConfig.name = config.getString("server.name", serverConfig.name);
        serverConfig.port = config.getInt("server.port", serverConfig.port);
        serverConfig.threadNum = config.getInt("server.thread_num", serverConfig.threadNum);
        serverConfig.metricsEnabled = config.getBool("metrics.enabled", serverConfig.metricsEnabled);
        serverConfig.storageType = config.getString("storage.type", serverConfig.storageType);
        serverConfig.statisticsEnabled =
            config.getBool("statistics.enabled", serverConfig.statisticsEnabled);
        serverConfig.mysqlHost = config.getString("mysql.host", serverConfig.mysqlHost);
        serverConfig.mysqlUser = config.getString("mysql.user", serverConfig.mysqlUser);
        serverConfig.mysqlPassword = config.getString("mysql.password", serverConfig.mysqlPassword);
        serverConfig.mysqlDatabase = config.getString("mysql.database", serverConfig.mysqlDatabase);
        serverConfig.mysqlPoolSize = config.getInt("mysql.pool_size", serverConfig.mysqlPoolSize);
        serverConfig.redisEnabled = config.getBool("redis.enabled", serverConfig.redisEnabled);
        serverConfig.redisHost = config.getString("redis.host", serverConfig.redisHost);
        serverConfig.redisPort = config.getInt("redis.port", serverConfig.redisPort);
        serverConfig.redisDatabase = config.getInt("redis.database", serverConfig.redisDatabase);
        serverConfig.redisTtlSeconds = config.getInt("redis.ttl_seconds", serverConfig.redisTtlSeconds);
        serverConfig.redisKeyPrefix = config.getString("redis.key_prefix", serverConfig.redisKeyPrefix);
        serverConfig.rateLimitEnabled = config.getBool("rate_limit.enabled", serverConfig.rateLimitEnabled);
        serverConfig.rateLimitRequests = config.getInt("rate_limit.requests", serverConfig.rateLimitRequests);
        serverConfig.rateLimitWindowSeconds =
            config.getInt("rate_limit.window_seconds", serverConfig.rateLimitWindowSeconds);
        serverConfig.rateLimitKeyPrefix =
            config.getString("rate_limit.key_prefix", serverConfig.rateLimitKeyPrefix);
        serverConfig.kafkaEnabled = config.getBool("kafka.enabled", serverConfig.kafkaEnabled);
        serverConfig.kafkaBootstrapServers =
            config.getString("kafka.bootstrap_servers", serverConfig.kafkaBootstrapServers);
        serverConfig.kafkaTopic = config.getString("kafka.topic", serverConfig.kafkaTopic);
        serverConfig.kafkaClientId = config.getString("kafka.client_id", serverConfig.kafkaClientId);
        serverConfig.kafkaQueueMaxMessages =
            config.getInt("kafka.queue_max_messages", serverConfig.kafkaQueueMaxMessages);
        serverConfig.kafkaMessageTimeoutMs =
            config.getInt("kafka.message_timeout_ms", serverConfig.kafkaMessageTimeoutMs);
        serverConfig.kafkaLingerMs = config.getInt("kafka.linger_ms", serverConfig.kafkaLingerMs);
        serverConfig.kafkaShutdownTimeoutMs =
            config.getInt("kafka.shutdown_timeout_ms", serverConfig.kafkaShutdownTimeoutMs);
        serverConfig.registrationEnabled =
            config.getBool("auth.registration_enabled", serverConfig.registrationEnabled);
        serverConfig.authCookieSecure =
            config.getBool("auth.cookie_secure", serverConfig.authCookieSecure);
        serverConfig.sessionTtlSeconds =
            config.getInt("auth.session_ttl_seconds", serverConfig.sessionTtlSeconds);
    }

    if (serverConfig.port <= 0 || serverConfig.port > 65535)
    {
        std::cerr << "Invalid server.port. Using default port 8080." << std::endl;
        serverConfig.port = 8080;
    }
    if (serverConfig.threadNum <= 0)
    {
        std::cerr << "Invalid server.thread_num. Using default thread_num 4." << std::endl;
        serverConfig.threadNum = 4;
    }
    if (serverConfig.storageType != "memory" && serverConfig.storageType != "mysql")
    {
        std::cerr << "Invalid storage.type. Using memory storage." << std::endl;
        serverConfig.storageType = "memory";
    }
    if (serverConfig.mysqlPoolSize <= 0)
    {
        std::cerr << "Invalid mysql.pool_size. Using default pool_size 4." << std::endl;
        serverConfig.mysqlPoolSize = 4;
    }
    if (serverConfig.statisticsEnabled && serverConfig.storageType != "mysql")
    {
        std::cerr << "statistics.enabled requires storage.type=mysql. Disabling statistics API."
                  << std::endl;
        serverConfig.statisticsEnabled = false;
    }
    if (serverConfig.redisPort <= 0 || serverConfig.redisPort > 65535)
    {
        std::cerr << "Invalid redis.port. Using default port 6379." << std::endl;
        serverConfig.redisPort = 6379;
    }
    if (serverConfig.redisDatabase < 0)
    {
        std::cerr << "Invalid redis.database. Using default database 0." << std::endl;
        serverConfig.redisDatabase = 0;
    }
    if (serverConfig.redisTtlSeconds <= 0 || serverConfig.redisTtlSeconds > 86400)
    {
        std::cerr << "redis.ttl_seconds must be between 1 and 86400. Using 3600." << std::endl;
        serverConfig.redisTtlSeconds = 3600;
    }
    if (serverConfig.rateLimitRequests <= 0 || serverConfig.rateLimitWindowSeconds <= 0 ||
        serverConfig.rateLimitKeyPrefix.empty() ||
        serverConfig.rateLimitKeyPrefix == serverConfig.redisKeyPrefix)
    {
        if (serverConfig.rateLimitEnabled)
        {
            std::cerr << "Invalid rate limit configuration. Disabling rate limiting." << std::endl;
        }
        serverConfig.rateLimitEnabled = false;
    }
    if (serverConfig.kafkaBootstrapServers.empty() || serverConfig.kafkaTopic.empty() ||
        serverConfig.kafkaClientId.empty() || serverConfig.kafkaQueueMaxMessages <= 0 ||
        serverConfig.kafkaMessageTimeoutMs <= 0 || serverConfig.kafkaLingerMs < 0 ||
        serverConfig.kafkaShutdownTimeoutMs < 0)
    {
        if (serverConfig.kafkaEnabled)
        {
            std::cerr << "Invalid Kafka configuration. Disabling Kafka access events." << std::endl;
        }
        serverConfig.kafkaEnabled = false;
    }
    if (serverConfig.sessionTtlSeconds < 300 || serverConfig.sessionTtlSeconds > 2592000)
    {
        std::cerr << "auth.session_ttl_seconds must be between 300 and 2592000. Using 604800."
                  << std::endl;
        serverConfig.sessionTtlSeconds = 604800;
    }
    return serverConfig;
}

} // namespace shortlink
