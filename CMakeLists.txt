cmake_minimum_required(VERSION 3.20)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
)

FetchContent_MakeAvailable(
    nlohmann_json
    googletest
    spdlog)

find_package(PostgreSQL REQUIRED)

add_library(
    matching_engine_lib
    src/order.cpp
    src/trade.cpp
    src/command_parser.cpp
    src/logger.cpp
    src/order_book.cpp
    src/matching_engine.cpp
    src/report_printer.cpp
    src/application.cpp
    src/pg_connection.cpp
    src/pg_result.cpp
    src/pg_transaction.cpp
)

target_include_directories(
    matching_engine_lib
    PUBLIC
    include
)

target_link_libraries(
    matching_engine_lib
    PUBLIC
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    PostgreSQL::PostgreSQL
)

add_executable(
    matching_engine
    src/main.cpp
)

target_link_libraries(
    matching_engine
    PRIVATE
    matching_engine_lib
)

enable_testing()

add_executable(
    matching_engine_tests
    tests/order_tests.cpp
    tests/command_parser_tests.cpp
    tests/order_book_tests.cpp
    tests/matching_engine_tests.cpp
    tests/pg_wrapper_tests.cpp
)

target_include_directories(
    matching_engine_tests
    PRIVATE
    include
)

target_link_libraries(
    matching_engine_tests
    PRIVATE
    matching_engine_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(matching_engine_tests)


