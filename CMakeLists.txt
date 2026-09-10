cmake_minimum_required(VERSION 3.20)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQ-TSAN-01, REQ-TSAN-02: отдельная конфигурация сборки под ThreadSanitizer,
# включаемая опцией -DENABLE_TSAN=ON и не затрагивающая обычную сборку.
# Флаги идут и в компиляцию, и в компоновку одним и тем же списком —
# ThreadSanitizer требует линковать runtime, а -g и -fno-omit-frame-pointer
# нужны ему для читаемых стеков.
option(ENABLE_TSAN "Собирать с ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    # Без явного типа сборки CMake не добавляет ни одного -O: на свежем
    # клоне у основной конфигурации (build/) умолчания типа сборки в
    # репозитории тоже нет, поэтому полагаться на "совпадает с build/" нельзя
    # — совпадение было бы случайностью локального кэша. Фиксируем
    # RelWithDebInfo (-O2 -g) явно и безусловно через CACHE FORCE, чтобы
    # результат прогона под санитайзером не зависел от того, задал ли
    # вызывающий тип сборки сам, и чтобы build-tsan/CMakeCache.txt отражал
    # реально используемые флаги, а не оставался пустым при живых -O2.
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
    set(TSAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)
    add_compile_options(${TSAN_FLAGS})
    add_link_options(${TSAN_FLAGS})
endif()

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
    src/response_serializer.cpp
    src/request_router.cpp
    src/session.cpp
    src/server.cpp
    src/client.cpp
    src/signal_handler.cpp
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
    tests/signal_handler_tests.cpp
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
# Цель недоступна в сборке с ENABLE_TSAN: Valgrind и ThreadSanitizer
# несовместимы (инструментированный ThreadSanitizer-рантайм под Valgrind
# падает) — без этого ограничения `cmake --build build-tsan --target
# memcheck` давал бы отказ, который выглядит как дефект памяти, а не как
# несовместимость инструментов.
if(NOT ENABLE_TSAN)
    add_custom_target(
        memcheck
        COMMAND valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1
            $<TARGET_FILE:matching_engine_tests>
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS matching_engine_tests
        COMMENT "Running matching_engine_tests under Valgrind memcheck"
    )
endif()


