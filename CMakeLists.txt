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
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

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
    src/config.cpp
    src/schema.cpp
    src/sequence_generator.cpp
    src/order_repository.cpp
    src/trade_repository.cpp
    src/command_repository.cpp
    src/command_cache.cpp
    src/persistence_service.cpp
    src/command_processor.cpp
    src/recovery_service.cpp
    src/cli_args.cpp
    src/message_codec.cpp
    src/request_router.cpp
    src/session.cpp
    src/server.cpp
    src/client.cpp
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
    Boost::boost
    Threads::Threads
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

add_executable(
    matching-engine-server
    src/server_main.cpp
)

target_link_libraries(
    matching-engine-server
    PRIVATE
    matching_engine_lib
)

add_executable(
    matching-engine-client
    src/client_main.cpp
)

target_link_libraries(
    matching-engine-client
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
    tests/config_tests.cpp
    tests/application_tests.cpp
    tests/cli_args_tests.cpp
    tests/schema_tests.cpp
    tests/sequence_generator_tests.cpp
    tests/order_repository_tests.cpp
    tests/command_repository_tests.cpp
    tests/trade_repository_tests.cpp
    tests/command_cache_tests.cpp
    tests/command_processor_tests.cpp
    tests/integration_tests.cpp
    tests/message_codec_tests.cpp
    tests/request_router_tests.cpp
    tests/network_tests.cpp
)

target_include_directories(
    matching_engine_tests
    PRIVATE
    include
)

# ctest запускает тесты с рабочим каталогом build/, а не корнем репозитория,
# поэтому тесту схемы БД нужен абсолютный путь до database/ независимо от
# того, откуда запущен ctest.
target_compile_definitions(
    matching_engine_tests
    PRIVATE
    MATCHING_ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(
    matching_engine_tests
    PRIVATE
    matching_engine_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(matching_engine_tests)

# REQ-OPT-01: запускает весь набор тестов под Valgrind одной командой сборки.
# ctest -T memcheck здесь не годится: найденные дефекты не влияют на код
# возврата ctest, и таргет был бы молча зелёным всегда. Поэтому valgrind
# вызывается напрямую на бинарнике тестов, а --error-exitcode=1 превращает
# любую найденную ошибку (включая утечки — при --leak-check=full они тоже
# считаются ошибками) в ненулевой код возврата команды.
# WORKING_DIRECTORY = корень репозитория: Application::run применяет схему
# из относительного пути "database", и часть тестов рассчитывает на рабочий
# каталог, совпадающий с тем, что использует ctest.
add_custom_target(
    memcheck
    COMMAND valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1
        $<TARGET_FILE:matching_engine_tests>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS matching_engine_tests
    COMMENT "Running matching_engine_tests under Valgrind memcheck"
)


