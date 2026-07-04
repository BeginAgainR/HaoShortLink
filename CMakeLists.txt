cmake_minimum_required(VERSION 3.10)
project(HaoShortLink)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/HttpServer/include
    ${PROJECT_SOURCE_DIR}/apps/shortlink_server/include
    /usr/include/mysql-cppconn-8
    /usr/include/mysql
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

target_link_libraries(shortlink_server
    pthread
    muduo_net
    muduo_base
    mysqlcppconn
    mysqlclient
    ssl
    crypto
)

message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Main source file: ${SHORTLINK_SERVER_MAIN}")
message(STATUS "ShortLink Server sources: ${SHORTLINK_SERVER_SRC}")
message(STATUS "HTTP Server sources: ${HTTP_SERVER_SRC}")
