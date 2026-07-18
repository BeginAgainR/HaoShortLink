cmake_minimum_required(VERSION 3.10)
project(HaoShortLink)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)
find_path(RDKAFKA_INCLUDE_DIR librdkafka/rdkafka.h)
find_library(RDKAFKA_LIBRARY rdkafka)

if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "hiredis development files are required")
endif()

if (NOT RDKAFKA_INCLUDE_DIR OR NOT RDKAFKA_LIBRARY)
    message(FATAL_ERROR "librdkafka development files are required")
endif()

include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/HttpServer/include
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/include
    /usr/include/mysql-cppconn-8
    /usr/include/mysql
    ${HIREDIS_INCLUDE_DIR}
    ${RDKAFKA_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

file(GLOB_RECURSE HTTP_SERVER_SRC
    "${PROJECT_SOURCE_DIR}/HttpServer/src/*.cpp"
)

set(SHORTLINK_SERVER_MAIN
    "${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/main.cpp"
)

file(GLOB_RECURSE SHORTLINK_SERVER_SRC
    "${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/*.cpp"
)
list(REMOVE_ITEM SHORTLINK_SERVER_SRC ${SHORTLINK_SERVER_MAIN})

add_executable(shortlink_server
    ${SHORTLINK_SERVER_MAIN}
    ${SHORTLINK_SERVER_SRC}
    ${HTTP_SERVER_SRC}
)

add_executable(shortlink_event_consumer
    ${PROJECT_SOURCE_DIR}/apps/shortlink_event_consumer/src/main.cpp
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/src/AccessEvent.cpp
    ${PROJECT_SOURCE_DIR}/HttpServer/src/utils/Config.cpp
)

target_link_libraries(shortlink_server
    pthread
    muduo_net
    muduo_base
    mysqlcppconn
    mysqlclient
    ${HIREDIS_LIBRARY}
    ${RDKAFKA_LIBRARY}
    ssl
    crypto
    nlohmann_json::nlohmann_json
)

target_link_libraries(shortlink_event_consumer
    pthread
    muduo_base
    ${RDKAFKA_LIBRARY}
    nlohmann_json::nlohmann_json
)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Main source file: ${SHORTLINK_SERVER_MAIN}")
message(STATUS "ShortLink Server sources: ${SHORTLINK_SERVER_SRC}")
message(STATUS "HTTP Server sources: ${HTTP_SERVER_SRC}")
