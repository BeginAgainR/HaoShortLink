#include "http/HttpServer.h"
#include "shortlink/AccessEventPublisher.h"
#include "shortlink/AccessStatisticsRepository.h"
#include "shortlink/AuthRepository.h"
#include "shortlink/AuthService.h"
#include "shortlink/KafkaAccessEventPublisher.h"
#include "shortlink/MemoryAuthRepository.h"
#include "shortlink/MemoryShortLinkRepository.h"
#include "shortlink/MySqlAccessStatisticsRepository.h"
#include "shortlink/MySqlAuthRepository.h"
#include "shortlink/MySqlShortLinkRepository.h"
#include "shortlink/RateLimiter.h"
#include "shortlink/RedisCachedShortLinkRepository.h"
#include "shortlink/RedisFixedWindowRateLimiter.h"
#include "shortlink/RedisShortLinkCache.h"
#include "shortlink/ServerConfig.h"
#include "shortlink/ShortLinkHttpApi.h"
#include "shortlink/ShortLinkMetrics.h"
#include "shortlink/ShortLinkRepository.h"
#include "shortlink/ShortLinkService.h"
#include "utils/db/DbConnectionPool.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <muduo/base/Logging.h>
#include <pthread.h>

int main(int argc, char* argv[])
{
    sigset_t terminationSignals;
    sigemptyset(&terminationSignals);
    sigaddset(&terminationSignals, SIGINT);
    sigaddset(&terminationSignals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &terminationSignals, nullptr) != 0)
    {
        std::cerr << "Failed to block termination signals." << std::endl;
        return 1;
    }

    const std::string configPath = argc > 1
                                       ? argv[1]
                                       : "apps/shortlink_server/config/server.conf.example";
    const shortlink::ServerConfig config = shortlink::loadServerConfig(configPath);
    LOG_INFO << "Starting " << config.name << " on port " << config.port
             << " with " << config.threadNum << " worker threads";

    shortlink::ShortLinkMetrics metrics;
    const shortlink::ShortLinkMetrics::Storage metricsStorage =
        config.storageType == "mysql"
            ? shortlink::ShortLinkMetrics::Storage::Mysql
            : shortlink::ShortLinkMetrics::Storage::Memory;

    std::unique_ptr<shortlink::ShortLinkRepository> primaryRepository;
    std::unique_ptr<shortlink::ShortLinkRepository> repository;
    std::unique_ptr<shortlink::AuthRepository> authRepository;
    std::unique_ptr<shortlink::AccessStatisticsRepository> statisticsRepository;

    if (config.storageType == "mysql")
    {
        http::db::DbConnectionPool::getInstance().init(
            config.mysqlHost,
            config.mysqlUser,
            config.mysqlPassword,
            config.mysqlDatabase,
            static_cast<std::size_t>(config.mysqlPoolSize));
        primaryRepository = std::make_unique<shortlink::MySqlShortLinkRepository>(&metrics);
        authRepository = std::make_unique<shortlink::MySqlAuthRepository>();
        if (config.statisticsEnabled)
        {
            statisticsRepository =
                std::make_unique<shortlink::MySqlAccessStatisticsRepository>(&metrics);
        }
        if (config.redisEnabled)
        {
            shortlink::RedisShortLinkCache::Config redisConfig;
            redisConfig.host = config.redisHost;
            redisConfig.port = config.redisPort;
            redisConfig.database = config.redisDatabase;
            redisConfig.ttlSeconds = config.redisTtlSeconds;
            redisConfig.keyPrefix = config.redisKeyPrefix;
            repository = std::make_unique<shortlink::RedisCachedShortLinkRepository>(
                *primaryRepository,
                shortlink::RedisShortLinkCache(redisConfig, &metrics),
                &metrics);
        }
        else
        {
            repository = std::move(primaryRepository);
        }
    }
    else
    {
        if (config.redisEnabled)
        {
            LOG_INFO << "redis.enabled is ignored when storage.type is not mysql";
        }
        repository = std::make_unique<shortlink::MemoryShortLinkRepository>(&metrics);
        authRepository = std::make_unique<shortlink::MemoryAuthRepository>();
    }

    std::unique_ptr<shortlink::RateLimiter> rateLimiter;
    if (config.rateLimitEnabled)
    {
        shortlink::RedisFixedWindowRateLimiter::Config limiterConfig;
        limiterConfig.host = config.redisHost;
        limiterConfig.port = config.redisPort;
        limiterConfig.database = config.redisDatabase;
        limiterConfig.requests = config.rateLimitRequests;
        limiterConfig.windowSeconds = config.rateLimitWindowSeconds;
        limiterConfig.keyPrefix = config.rateLimitKeyPrefix;
        rateLimiter =
            std::make_unique<shortlink::RedisFixedWindowRateLimiter>(std::move(limiterConfig));
    }

    std::unique_ptr<shortlink::AccessEventPublisher> accessEventPublisher =
        std::make_unique<shortlink::NoopAccessEventPublisher>();
    if (config.kafkaEnabled)
    {
        shortlink::KafkaAccessEventPublisher::Config kafkaConfig;
        kafkaConfig.bootstrapServers = config.kafkaBootstrapServers;
        kafkaConfig.topic = config.kafkaTopic;
        kafkaConfig.clientId = config.kafkaClientId;
        kafkaConfig.queueMaxMessages = config.kafkaQueueMaxMessages;
        kafkaConfig.messageTimeoutMs = config.kafkaMessageTimeoutMs;
        kafkaConfig.lingerMs = config.kafkaLingerMs;
        kafkaConfig.shutdownTimeoutMs = config.kafkaShutdownTimeoutMs;
        try
        {
            accessEventPublisher = std::make_unique<shortlink::KafkaAccessEventPublisher>(
                std::move(kafkaConfig),
                &metrics);
        }
        catch (const std::exception& error)
        {
            LOG_ERROR << "event=kafka_access_event_producer status=disabled fail_open=true"
                      << " reason=" << error.what();
        }
    }

    shortlink::ShortLinkService shortLinkService(*repository, &metrics);
    shortlink::AuthService authService(*authRepository, config.sessionTtlSeconds);
    http::HttpServer server(config.port, config.name);
    server.setThreadNum(config.threadNum);

    shortlink::ShortLinkHttpApiConfig apiConfig;
    apiConfig.requiresMySql = config.storageType == "mysql";
    apiConfig.metricsEnabled = config.metricsEnabled;
    apiConfig.metricsStorage = metricsStorage;
    apiConfig.auth.registrationEnabled = config.registrationEnabled;
    apiConfig.auth.cookieSecure = config.authCookieSecure;
    shortlink::registerShortLinkHttpApi(&server,
                                        &shortLinkService,
                                        &authService,
                                        &metrics,
                                        rateLimiter.get(),
                                        accessEventPublisher.get(),
                                        statisticsRepository.get(),
                                        apiConfig);

    std::thread shutdownThread([&server, &terminationSignals]() {
        int signal = 0;
        if (sigwait(&terminationSignals, &signal) == 0)
        {
            LOG_INFO << "event=server_shutdown signal=" << signal;
            server.getLoop()->quit();
        }
    });

    server.start();
    shutdownThread.join();
}
