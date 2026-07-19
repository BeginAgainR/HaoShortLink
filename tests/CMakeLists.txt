add_executable(hao_shortlink_tests
    minimal_test.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/AccessEvent.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/AccessStatistics.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_event_consumer/src/KafkaDeadLetterPublisher.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_event_consumer/src/ConsumerMetrics.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/MemoryShortLinkRepository.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/RedirectHandler.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/ShortLinkService.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/ShortLinkMetrics.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/ShortLinkRepository.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/http/HttpRequest.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/http/HttpResponse.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/middleware/MiddlewareChain.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/metrics/HttpMetrics.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/router/Router.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/utils/RequestId.cpp
)

target_link_libraries(hao_shortlink_tests
    pthread
    muduo_net
    muduo_base
    ${RDKAFKA_LIBRARY}
    nlohmann_json::nlohmann_json
)

add_test(NAME hao_shortlink_tests COMMAND hao_shortlink_tests)
