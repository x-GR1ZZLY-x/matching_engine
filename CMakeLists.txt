cmake_minimum_required(VERSION 3.20)
project(matching_engine VERSION 1.0.0 LANGUAGES CXX)

# Правила установки и пакет рассчитаны на префикс /usr, и это условие
# выражено здесь, а не оставлено на память вызывающего: unit-файл жёстко
# называет /usr/bin/matching-engine-server, а список conffiles —
# /etc/matching-engine/config.json. С умолчанием /usr/local те же правила
# разложили бы файлы в /usr/local/bin и /usr/local/etc, пакет собрался бы
# без единой жалобы и с верным именем, служба не нашла бы ни бинарника, ни
# конфигурации, а объявленный conffile отсутствовал бы в архиве вовсе.
# Условие срабатывает только когда префикс не задан явно: значение из
# командной строки не перекрывается.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQ-TSAN-01, REQ-TSAN-02: отдельная конфигурация сборки под ThreadSanitizer,
# включаемая опцией -DENABLE_TSAN=ON и не затрагивающая обычную сборку.
# Флаги идут и в компиляцию, и в компоновку одним и тем же списком —
# ThreadSanitizer требует линковать runtime, а -g и -fno-omit-frame-pointer
# нужны ему для читаемых стеков.
option(ENABLE_TSAN "Собирать с ThreadSanitizer" OFF)
if(ENABLE_TSAN)
    # Тип сборки под санитайзером фиксируется безусловно через CACHE FORCE:
    # RelWithDebInfo (-O2 -g), чтобы результат прогона не зависел от того,
    # задал ли вызывающий тип сам, и чтобы build-tsan/CMakeCache.txt отражал
    # реально используемые флаги. Это сознательно другой уровень оптимизации,
    # чем у основной конфигурации (Release, -O3, умолчание ниже): санитайзеру
    # нужна отладочная информация и сохранённые фрейм-поинтеры, а не
    # максимальная оптимизация.
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
    set(TSAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)
    add_compile_options(${TSAN_FLAGS})
    add_link_options(${TSAN_FLAGS})
endif()

# Тип сборки основной конфигурации по умолчанию — Release. Без этого блока
# тип задавала бы не конфигурация проекта, а зависимость: spdlog при пустом
# CMAKE_BUILD_TYPE выставляет Release через CACHE FORCE, и результат был бы
# верным по случайности — до первой смены или обновления библиотеки. Блок
# стоит до include(FetchContent), чтобы зависимости увидели уже заданное
# значение, и после блока санитайзера, где тип задан безусловно. Значение,
# переданное явно (-DCMAKE_BUILD_TYPE=...), не перекрывается.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(FetchContent)

# Дерево установки не должно содержать ничего, кроме бинарников, конфигурации,
# схемы и unit-файла. Существенна здесь первая строка: GoogleTest объявляет
# INSTALL_GTEST включённым по умолчанию, и без её отключения `cmake --install`
# и пакет .deb унесли бы с собой его заголовки и библиотеки. У spdlog и
# nlohmann_json установка включается только когда библиотека собирается сама
# по себе, а не подключена к чужому проекту, — то есть здесь уже выключена;
# строки оставлены, чтобы включение по умолчанию в новой версии любой из них
# не прошло незамеченным. Всё это обязано стоять до FetchContent_MakeAvailable.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)

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

# Установка (REQ-PKG-01) и сборка пакета .deb.
# Пакет собирается из того же дерева установки, которое даёт
# `cmake --install` — иначе состав пакета и результат ручной установки
# расходятся при первой же правке. Старый консольный бинарник
# matching_engine в установку не входит: он остаётся только в сборке ради
# тестов, службе он не нужен.
#
# Блок недоступен в конфигурации с ENABLE_TSAN, как и цель memcheck выше:
# иначе cpack из каталога сборки с санитайзером дал бы файл с тем же самым
# именем matching-engine_<версия>_amd64.deb, но с инструментированными
# бинарниками, которым нужен рантайм санитайзера. Два одинаково названных
# пакета с разным содержимым перепутать слишком легко, а обнаружилось бы это
# только при запуске установленной службы.
if(NOT ENABLE_TSAN)

include(GNUInstallDirs)

install(TARGETS matching-engine-server matching-engine-client
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# FILES_MATCHING: в пакет уходят только файлы схемы, а не всё содержимое
# каталога. Приложение применяет при старте каждый .sql, который там найдёт,
# поэтому черновая миграция или дамп, случайно оставшийся в рабочем дереве,
# был бы не мусором в пакете, а изменением, применённым к чужой базе.
install(DIRECTORY database/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/matching-engine/database
        FILES_MATCHING PATTERN "*.sql")

# CMAKE_INSTALL_SYSCONFDIR сам по себе остаётся относительным ("etc") при
# любом префиксе — раскрытие исключения "префикс /usr -> /etc, а не
# /usr/etc" выполняет только CMAKE_INSTALL_FULL_SYSCONFDIR (проверено:
# install(DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}) при -DCMAKE_INSTALL_PREFIX=/usr
# кладёт файлы в /usr/etc). Абсолютный путь из FULL_-варианта install()
# передаёт DESTDIR так же, как уже сделано для unit-файла ниже.
install(FILES packaging/config/config.json
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/matching-engine)

install(FILES packaging/config/matching-engine.env.example
        DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/matching-engine)

install(FILES packaging/systemd/matching-engine.service
        DESTINATION /lib/systemd/system)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "matching-engine")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Order matching engine TCP service")
# Развёрнутое описание задаётся переменной генератора DEB, а не общей:
# у общей есть цепочка запасных источников, и без явного значения описание
# пакета сводится к одной строке summary (проверено выводом dpkg-deb --info).
# Формат Debian требует продолжения строк с ведущим пробелом.
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "Order matching engine TCP service
 Order matching engine served over TCP with a length-prefixed framing
 protocol. Orders, trades and processed commands are stored in PostgreSQL,
 so the order book is restored on start and repeated commands return the
 response of their first execution. The package ships the server, a console
 client and a systemd unit.")
set(CPACK_PACKAGE_CONTACT "Matching Engine maintainer <maintainer@example.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "misc")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

# CPack сам не объявляет /etc/matching-engine/config.json файлом настроек
# (conffile) — без этого списка правки администратора будут молча затёрты
# при первом же обновлении пакета.
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/packaging/debian/conffiles")

# include(CPack) должен идти последней строкой: модуль читает переменные
# CPack в момент подключения, и значение, заданное после него, в пакет не
# попадёт.
include(CPack)

endif()
