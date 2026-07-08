add_executable(hao_shortlink_tests
    minimal_test.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/MemoryShortLinkRepository.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/ShortLinkService.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/http/HttpRequest.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/http/HttpResponse.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/middleware/MiddlewareChain.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/router/Router.cpp
)

target_link_libraries(hao_shortlink_tests
    pthread
    muduo_net
    muduo_base
)

add_test(NAME hao_shortlink_tests COMMAND hao_shortlink_tests)
