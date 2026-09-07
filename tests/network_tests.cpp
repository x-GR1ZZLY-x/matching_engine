#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <future>
#include <string>
#include <thread>

#include <memory>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "client.hpp"
#include "command.hpp"
#include "command_processor.hpp"
#include "config.hpp"
#include "exceptions.hpp"
#include "message_codec.hpp"
#include "order.hpp"
#include "pg_connection.hpp"
#include "pg_result.hpp"
#include "request_router.hpp"
#include "server.hpp"
#include "test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

namespace {

// Обёртка "сервер, поднятый в этом же процессе, в отдельном потоке
// io_context" (обязательное условие ТЗ — иначе до сервера не добраться ни
// тесту через реальный сокет, ни в будущем ThreadSanitizer). port = 0 —
// операционная система выбирает порт сама, реальный номер известен сразу
// после конструктора Server (bind/listen там синхронны), поэтому никакой
// синхронизации ожидания готовности не нужно.
struct TestServer {
    // connection == nullptr годится для тестов, которые не отправляют
    // ADD/CANCEL/MODIFY (PING, разбор кадра, PRINT над заявками, положенными
    // в книгу напрямую через processor.restoreOrder, минуя сеть и БД):
    // CommandProcessor не требует БД для конструирования, а PRINT читает
    // processor.orderBook() без обращения к connection_.
    // Команды, которые действительно пишут в БД, тестам нужен реальный
    // PgConnection извне (см. tryConnect() в test_database.hpp).
    explicit TestServer(std::size_t maxMessageSize, PgConnection* connection = nullptr)
        : router(processor, connection),
          config{"127.0.0.1", 0, maxMessageSize},
          server(ioContext, config, router) {
        server.start();
        // Исключение, покинувшее обработчик внутри io_context.run(), иначе
        // приводит к std::terminate() посреди прогона без внятного
        // сообщения (тот же случай, что уже обёрнут в server_main.cpp).
        ioThread = std::thread([this] {
            try {
                ioContext.run();
            } catch (const std::exception& e) {
                ADD_FAILURE() << "io_context::run() threw: " << e.what();
            }
        });
    }

    ~TestServer() {
        // Остановка выполняется прямым вызовом метода сервера, а не только
        // обрывом io_context: именно на Server::stop() (REQ-NET-08) ляжет
        // корректное завершение, и обрыв цикла событий проверял бы не его.
        server.stop();
        // io_context::stop() безопасен для вызова из чужого потока по
        // контракту Asio — здесь вызывается из потока теста, пока
        // io_context крутится в ioThread.
        ioContext.stop();
        // joinable() == false, если тест уже сам дождался остановки потока
        // (тесты фатального завершения ниже: сервис останавливает себя
        // изнутри io-потока после сбоя сохранения, и тест обязан
        // присоединиться к потоку до проверки hadFatalError() — повторный
        // join() того же std::thread иначе бросил бы std::system_error).
        if (ioThread.joinable()) {
            ioThread.join();
        }
    }

    unsigned short port() const { return server.port(); }

    // Порядок полей значим: server хранит ссылку на router и работает
    // через ioContext, поэтому обязан разрушаться раньше них обоих, а
    // деструкторы полей вызываются в порядке, обратном объявлению.
    // processor объявлен первым — router держит на него ссылку и обязан
    // конструироваться после (и разрушаться до) него.
    CommandProcessor processor;
    RequestRouter router;
    boost::asio::io_context ioContext;
    ServerConfig config;
    Server server;
    std::thread ioThread;
};

// Полностью очищает три таблицы персистентности — тот же порядок и то же
// обоснование, что и в tests/integration_tests.cpp: trades ссылается на
// orders внешним ключом, поэтому сначала сделки, потом заявки, затем
// обработанные команды.
void cleanupAllTables(PgConnection& connection) {
    connection.execute("DELETE FROM trades");
    connection.execute("DELETE FROM orders");
    connection.execute("DELETE FROM processed_commands");
}

// Выполняет fn внутри io_context сервера, а не напрямую из потока теста, и
// дожидается завершения через promise/future: OrderBook и CommandProcessor
// принадлежат сетевому потоку — тот же инвариант проекта, что и в остальном
// коде, — а прямой вызов processor.restoreOrder() из потока теста, пока
// io_context крутится в TestServer::ioThread, не имеет установленного
// happens-before между потоками. Тест, который сегодня не наблюдает гонки,
// ThreadSanitizer под ENABLE_TSAN может отловить не в каждом прогоне.
//
// future.wait() ограничен таймаутом: правила сетевых тестов запрещают
// ожидание без предела по времени — если io_context уже остановлен (или
// вовсе не запущен), задача, поставленная через post(), никогда не
// выполнится, и это обязано быть падением теста, а не зависанием.
void runInIoContext(TestServer& testServer, const std::function<void()>& fn) {
    std::promise<void> done;
    std::future<void> future = done.get_future();
    boost::asio::post(testServer.ioContext, [&fn, &done] {
        fn();
        done.set_value();
    });
    constexpr std::chrono::seconds kTimeout(5);
    ASSERT_EQ(future.wait_for(kTimeout), std::future_status::ready)
        << "задача не выполнилась в io_context сервера за отведённое время";
}

// Присоединяется к io-потоку сервера, ожидая, что тот остановится сам
// (Server::onFatalShutdown_ после сбоя сохранения). Каждый сетевой тест
// обязан иметь ограничение по времени и падать, а не висеть, — это верно
// и для теста, который специально проверяет отсутствующую остановку: если
// сервис не остановился сам за отведённое время, сторожевой поток
// останавливает io_context принудительно, join() завершается, и тест
// падает на последующем EXPECT_TRUE(hadFatalError()), а не зависает
// навсегда.
//
// Проверка joinable() выполняется до создания сторожевого потока: ранний
// выход по ASSERT_TRUE после watchdog оставил бы его неприсоединённым, и
// вместо понятного красного теста упал бы весь тестовый бинарник на
// std::terminate() из деструктора неприсоединённого std::thread.
void joinIoThreadWithTimeout(TestServer& testServer, std::chrono::seconds timeout) {
    ASSERT_TRUE(testServer.ioThread.joinable());

    std::promise<void> done;
    std::shared_future<void> doneFuture = done.get_future();
    std::thread watchdog([&testServer, doneFuture, timeout] {
        if (doneFuture.wait_for(timeout) == std::future_status::timeout) {
            testServer.server.stop();
            testServer.ioContext.stop();
        }
    });
    testServer.ioThread.join();
    done.set_value();
    watchdog.join();
}

}

// Критерий 3: PING без обращения к разбору команд предметной области.
TEST(NetworkTest, PingReturnsPong) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());
    const nlohmann::json response = client.request(nlohmann::json::parse(R"({"type":"PING"})"));

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("result"), "PONG");
    EXPECT_EQ(response.size(), 2u);
}

// Неизвестный тип команды — обычная ошибка пользователя: ответ со статусом
// ERROR и кодом INVALID_REQUEST (docs/task4/02-network-protocol.md, раздел
// 3.5), соединение при этом живёт.
TEST(NetworkTest, UnknownCommandTypeReturnsInvalidRequest) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());
    // Тип заведомо неизвестный: "ADD" здесь стоял, пока команды предметной
    // области не были подключены, и после их подключения проверял бы уже не
    // неизвестный тип, а провал разбора команды — совсем другую ветку.
    const nlohmann::json response = client.request(nlohmann::json::parse(R"({"type":"FROB"})"));

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
}

// Критерий 6: кадр отправлен четырьмя частями — часть заголовка, остаток
// заголовка, часть тела, остаток тела.
TEST(NetworkTest, FragmentedFrameIsAssembledCorrectly) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    const std::string frame = client.encodeFrame(R"({"type":"PING"})");
    ASSERT_GT(frame.size(), kFrameHeaderSize + 4);

    client.sendRawBytes(frame.substr(0, 2));
    client.sendRawBytes(frame.substr(2, kFrameHeaderSize - 2));
    const std::size_t bodySize = frame.size() - kFrameHeaderSize;
    const std::size_t bodyHalf = bodySize / 2;
    client.sendRawBytes(frame.substr(kFrameHeaderSize, bodyHalf));
    client.sendRawBytes(frame.substr(kFrameHeaderSize + bodyHalf));

    const nlohmann::json response = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("result"), "PONG");
}

// Критерий 7: три кадра одним вызовом записи, три ответа в том же
// порядке. Три разных (несуществующих) типа команд дают три разных
// сообщения об ошибке — так порядок ответов проверяется по содержимому, а
// не только по счётчику полученных кадров.
TEST(NetworkTest, ThreeFramesInOneWriteProduceThreeResponsesInOrder) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    const std::string frameA = client.encodeFrame(R"({"type":"AAA"})");
    const std::string frameB = client.encodeFrame(R"({"type":"BBB"})");
    const std::string frameC = client.encodeFrame(R"({"type":"CCC"})");
    client.sendRawBytes(frameA + frameB + frameC);

    const nlohmann::json responseA = nlohmann::json::parse(client.receiveFrame());
    const nlohmann::json responseB = nlohmann::json::parse(client.receiveFrame());
    const nlohmann::json responseC = nlohmann::json::parse(client.receiveFrame());

    EXPECT_NE(responseA.at("message").get<std::string>().find("AAA"), std::string::npos);
    EXPECT_NE(responseB.at("message").get<std::string>().find("BBB"), std::string::npos);
    EXPECT_NE(responseC.at("message").get<std::string>().find("CCC"), std::string::npos);
}

// Критерий 8: заголовок объявляет размер больше server.max_message_size.
// Клиентский кодек намеренно с бОльшим пределом, чем у сервера, — иначе
// encodeFrame отверг бы кадр сам, и тест проверял бы клиента, а не сервер.
TEST(NetworkTest, OversizedMessageIsRejectedConnectionClosesServerStaysAlive) {
    // 32 байта меньше самой короткой осмысленной команды — лимит поднят до
    // реалистичного значения, чтобы тест проверял превышение размера, а не
    // отказывал на любом сообщении вообще.
    constexpr std::size_t kServerLimit = 256;
    TestServer testServer(kServerLimit);

    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    nlohmann::json bigRequest;
    bigRequest["type"] = "PING";
    // Payload обязан превышать kServerLimit целиком, с учётом обрамления
    // JSON, а не только длины padding.
    bigRequest["padding"] = std::string(300, 'x');
    client.sendRawBytes(client.encodeFrame(bigRequest.dump()));

    const nlohmann::json errorResponse = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(errorResponse.at("status"), "ERROR");
    EXPECT_EQ(errorResponse.at("error"), "MESSAGE_TOO_LARGE");

    // Соединение закрыто сервером сразу после ответа — следующее чтение
    // обязано провалиться обрывом, а не просто отсутствием ответа: обрыв и
    // таймаут различаются типом исключения (NetworkTimeoutError — частный
    // случай NetworkError), иначе эта проверка была бы зелёной и при
    // сервере, который просто молчит, оставив сокет открытым.
    try {
        client.receiveFrame();
        FAIL() << "сервер обязан был закрыть соединение";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение осталось открытым: сервер молчит вместо закрытия";
    } catch (const NetworkError&) {
        SUCCEED();
    }

    // Сервер остался жив и обслуживает следующего клиента.
    Client another(1024);
    another.connect("127.0.0.1", testServer.port());
    const nlohmann::json pong = another.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pong.at("status"), "OK");
    EXPECT_EQ(pong.at("result"), "PONG");
}

// MESSAGE_TOO_LARGE обязан встать в очередь
// записи вслед за уже отправляемым ответом, а не оборвать соединение
// прямо в closeAfterMessageTooLarge — иначе на живом клиенте потерялся бы
// предыдущий ответ. Валидный PING и следом кадр с заголовком, объявляющим
// размер больше лимита, отправляются одной записью: сервер обязан сначала
// полностью ответить PONG и только затем — MESSAGE_TOO_LARGE, и лишь после
// этого закрыть соединение.
TEST(NetworkTest, MessageTooLargeIsQueuedBehindPendingResponseBeforeClosing) {
    constexpr std::size_t kServerLimit = 256;
    TestServer testServer(kServerLimit);

    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    const std::string pingFrame = client.encodeFrame(R"({"type":"PING"})");

    nlohmann::json bigRequest;
    bigRequest["type"] = "PING";
    bigRequest["padding"] = std::string(300, 'x');
    const std::string oversizedFrame = client.encodeFrame(bigRequest.dump());

    client.sendRawBytes(pingFrame + oversizedFrame);

    const nlohmann::json pong = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(pong.at("status"), "OK");
    EXPECT_EQ(pong.at("result"), "PONG");

    const nlohmann::json errorResponse = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(errorResponse.at("status"), "ERROR");
    EXPECT_EQ(errorResponse.at("error"), "MESSAGE_TOO_LARGE");

    // Обрыв различается от таймаута по типу исключения — тем же способом,
    // что и в тесте превышения размера выше.
    try {
        client.receiveFrame();
        FAIL() << "сервер обязан был закрыть соединение";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение осталось открытым: сервер молчит вместо закрытия";
    } catch (const NetworkError&) {
        SUCCEED();
    }
}

// Критерий 9: битый JSON не разрывает соединение — следующая команда в
// том же соединении выполняется успешно.
TEST(NetworkTest, InvalidJsonInSameConnectionThenPingSucceeds) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    client.sendRawBytes(client.encodeFrame("this is not json"));
    const nlohmann::json errorResponse = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(errorResponse.at("status"), "ERROR");
    EXPECT_EQ(errorResponse.at("error"), "INVALID_REQUEST");

    const nlohmann::json pingResponse = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pingResponse.at("status"), "OK");
    EXPECT_EQ(pingResponse.at("result"), "PONG");
}

// Критерий 10: клиент отключился, не дочитав ответ, — сервер не падает,
// следующее подключение обслуживается.
TEST(NetworkTest, ClientDisconnectWithoutReadingResponseDoesNotCrashServer) {
    TestServer testServer(1024);

    {
        Client client(1024);
        client.connect("127.0.0.1", testServer.port());
        client.sendRawBytes(client.encodeFrame(R"({"type":"PING"})"));
        client.close();
    }

    Client another(1024);
    another.connect("127.0.0.1", testServer.port());
    const nlohmann::json response = another.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("result"), "PONG");
}

// Прямой вызов Server::stop() (REQ-NET-08): закрывает
// acceptor, новые подключения больше не принимаются, но уже открытая
// сессия продолжает отвечать — stop() не трогает существующие соединения.
TEST(NetworkTest, StopClosesAcceptorButExistingSessionKeepsWorking) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    // Первый обмен подтверждает, что соединение уже полностью принято
    // сервером (async_accept отработал, Session::start() запущен) до того,
    // как acceptor будет закрыт. Без этого round trip'а TCP-рукопожатие на
    // уровне ОС могло бы завершиться раньше, чем сервер успел вызвать
    // accept(), и закрытие слушающего сокета оборвало бы ещё не принятое
    // соединение той же ошибкой, что и настоящий сбой.
    const nlohmann::json warmup = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    ASSERT_EQ(warmup.at("status"), "OK");

    testServer.server.stop();

    const nlohmann::json pong = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pong.at("status"), "OK");
    EXPECT_EQ(pong.at("result"), "PONG");

    // NetworkTimeoutError наследует NetworkError, поэтому EXPECT_THROW с
    // базовым типом пропустил бы регрессию, при которой acceptor не закрыт
    // и подключение просто повисает до таймаута, вместо того чтобы
    // получить немедленный отказ (та же асимметрия, что уже устранена в
    // тесте превышения размера).
    Client another(1024);
    try {
        another.connect("127.0.0.1", testServer.port());
        FAIL() << "acceptor обязан был отказать в новом подключении";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "подключение зависло до таймаута: acceptor не закрыт";
    } catch (const NetworkError&) {
        SUCCEED();
    }
}

// Команды предметной области, ResponseSerializer, PRINT и идемпотентный
// ответ по сети (REQ-API-01/02/03/06/07/08/09/10).

// Критерий 1: ADD BUY 100x10, затем ADD SELL 100x4 — ответ на вторую
// команду содержит ровно одну сделку с ожидаемыми полями.
TEST(NetworkTest, AddThenAddProducesTradeWithOrderId) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    TestServer testServer(4096, &*connOpt);
    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    nlohmann::json buy;
    buy["type"] = "ADD";
    buy["order_id"] = 1;
    buy["side"] = "BUY";
    buy["price"] = 100;
    buy["quantity"] = 10;
    buy["command_id"] = "cmd-network-1001";
    ASSERT_EQ(client.request(buy).at("status"), "OK");

    nlohmann::json sell;
    sell["type"] = "ADD";
    sell["order_id"] = 2;
    sell["side"] = "SELL";
    sell["price"] = 100;
    sell["quantity"] = 4;
    sell["command_id"] = "cmd-network-1002";
    const nlohmann::json sellResponse = client.request(sell);

    EXPECT_EQ(sellResponse.at("status"), "OK");
    EXPECT_EQ(sellResponse.at("order_id"), 2);
    ASSERT_EQ(sellResponse.at("trades").size(), 1u);
    const auto& trade = sellResponse.at("trades")[0];
    EXPECT_EQ(trade.at("buy_order_id"), 1);
    EXPECT_EQ(trade.at("sell_order_id"), 2);
    EXPECT_EQ(trade.at("price"), 100);
    EXPECT_EQ(trade.at("quantity"), 4);
}

// Критерий 2: пример запроса из текста задания, вставленный дословно (поле
// order_id, а не id) — обе команды обрабатываются успешно, ответы совпадают
// буквально с примером из "Проект (2).md".
TEST(NetworkTest, LiteralTaskExampleAddSequenceSucceeds) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    TestServer testServer(4096, &*connOpt);
    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    const nlohmann::json first = client.request(nlohmann::json::parse(R"({
        "command_id": "cmd-1001",
        "type": "ADD",
        "order_id": 1,
        "side": "BUY",
        "price": 100,
        "quantity": 10
    })"));
    EXPECT_EQ(first.at("command_id"), "cmd-1001");
    EXPECT_EQ(first.at("status"), "OK");
    EXPECT_EQ(first.at("order_id"), 1);
    EXPECT_TRUE(first.at("trades").empty());

    const nlohmann::json second = client.request(nlohmann::json::parse(R"({
        "command_id": "cmd-1002",
        "type": "ADD",
        "order_id": 2,
        "side": "SELL",
        "price": 100,
        "quantity": 4
    })"));
    EXPECT_EQ(second.at("command_id"), "cmd-1002");
    EXPECT_EQ(second.at("status"), "OK");
    EXPECT_EQ(second.at("order_id"), 2);
    ASSERT_EQ(second.at("trades").size(), 1u);
    EXPECT_EQ(second.at("trades")[0].at("buy_order_id"), 1);
    EXPECT_EQ(second.at("trades")[0].at("sell_order_id"), 2);
    EXPECT_EQ(second.at("trades")[0].at("price"), 100);
    EXPECT_EQ(second.at("trades")[0].at("quantity"), 4);
}

// Критерий 3: поле "id" вместо "order_id" — синоним из консольного режима
// прошлых работ принимается так же, как и новое имя (REQ-API-10).
TEST(NetworkTest, IdFieldNameIsAcceptedAsOrderIdSynonym) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    TestServer testServer(4096, &*connOpt);
    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    nlohmann::json request;
    request["type"] = "ADD";
    request["id"] = 42;
    request["side"] = "BUY";
    request["price"] = 100;
    request["quantity"] = 5;
    request["command_id"] = "cmd-id-synonym";

    const nlohmann::json response = client.request(request);
    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("order_id"), 42);
}

// Критерий 4: CANCEL несуществующей заявки — status ERROR, error
// ORDER_NOT_FOUND, непустой message, оба поля лежат на верхнем уровне
// (а не во вложенном объекте ошибки).
TEST(NetworkTest, CancelNonexistentOrderReturnsOrderNotFound) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    TestServer testServer(4096, &*connOpt);
    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    nlohmann::json cancel;
    cancel["type"] = "CANCEL";
    cancel["order_id"] = 999999;
    cancel["command_id"] = "cmd-55";

    const nlohmann::json response = client.request(cancel);
    EXPECT_EQ(response.at("status"), "ERROR");
    ASSERT_TRUE(response.at("error").is_string());
    EXPECT_EQ(response.at("error"), "ORDER_NOT_FOUND");
    ASSERT_TRUE(response.at("message").is_string());
    EXPECT_FALSE(response.at("message").get<std::string>().empty());
}

// Критерий 5: после ответа об ошибке в том же соединении следующая
// корректная команда выполняется успешно — ошибка пользователя не рвёт
// соединение (REQ-API-07).
TEST(NetworkTest, ErrorThenValidCommandInSameConnectionSucceeds) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    TestServer testServer(4096, &*connOpt);
    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    nlohmann::json cancel;
    cancel["type"] = "CANCEL";
    cancel["order_id"] = 424242;
    cancel["command_id"] = "cmd-err";
    ASSERT_EQ(client.request(cancel).at("status"), "ERROR");

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = 501;
    add["side"] = "BUY";
    add["price"] = 50;
    add["quantity"] = 3;
    add["command_id"] = "cmd-ok";
    const nlohmann::json okResponse = client.request(add);
    EXPECT_EQ(okResponse.at("status"), "OK");
    EXPECT_EQ(okResponse.at("order_id"), 501);
}

// Критерий 6: MODIFY и рыночная заявка (order_type: MARKET) работают через
// сетевой API. Модифицированная BUY 10x101 теряет временной приоритет
// (remove + re-add), а встречная MARKET SELL исполняется по цене книжной
// заявки, а не по своей — у MARKET цены нет вовсе.
TEST(NetworkTest, ModifyAndMarketOrderWorkOverNetwork) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    TestServer testServer(4096, &*connOpt);
    Client client(4096);
    client.connect("127.0.0.1", testServer.port());

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = 10;
    add["side"] = "BUY";
    add["price"] = 100;
    add["quantity"] = 5;
    add["command_id"] = "cmd-modify-add-10";
    ASSERT_EQ(client.request(add).at("status"), "OK");

    nlohmann::json modify;
    modify["type"] = "MODIFY";
    modify["order_id"] = 10;
    modify["price"] = 101;
    modify["quantity"] = 8;
    modify["command_id"] = "cmd-modify-10";
    const nlohmann::json modifyResponse = client.request(modify);
    EXPECT_EQ(modifyResponse.at("status"), "OK");
    EXPECT_EQ(modifyResponse.at("order_id"), 10);

    nlohmann::json marketSell;
    marketSell["type"] = "ADD";
    marketSell["order_type"] = "MARKET";
    marketSell["order_id"] = 11;
    marketSell["side"] = "SELL";
    marketSell["quantity"] = 3;
    marketSell["command_id"] = "cmd-market-11";
    const nlohmann::json marketResponse = client.request(marketSell);

    EXPECT_EQ(marketResponse.at("status"), "OK");
    EXPECT_EQ(marketResponse.at("order_id"), 11);
    ASSERT_EQ(marketResponse.at("trades").size(), 1u);
    EXPECT_EQ(marketResponse.at("trades")[0].at("buy_order_id"), 10);
    EXPECT_EQ(marketResponse.at("trades")[0].at("sell_order_id"), 11);
    EXPECT_EQ(marketResponse.at("trades")[0].at("price"), 101);
    EXPECT_EQ(marketResponse.at("trades")[0].at("quantity"), 3);
}

// Критерий 7: PRINT возвращает книгу в формате раздела 3.4 контракта, и
// порядок элементов соответствует приоритету исполнения. Книга заполняется
// напрямую через CommandProcessor::restoreOrder — без сети и без БД, тем же
// способом, каким RecoveryService поднимает книгу при старте сервера.
TEST(NetworkTest, PrintReturnsBookInPriorityOrder) {
    TestServer testServer(4096);

    runInIoContext(testServer, [&testServer] {
        testServer.processor.restoreOrder(std::make_shared<Order>(
            1, Side::Buy, 100, 5, 5, 1, OrderStatus::Open));
        testServer.processor.restoreOrder(std::make_shared<Order>(
            2, Side::Buy, 100, 3, 3, 2, OrderStatus::Open));
        testServer.processor.restoreOrder(std::make_shared<Order>(
            3, Side::Buy, 101, 1, 1, 3, OrderStatus::Open));
        testServer.processor.restoreOrder(std::make_shared<Order>(
            4, Side::Sell, 105, 2, 2, 4, OrderStatus::Open));
        testServer.processor.restoreOrder(std::make_shared<Order>(
            5, Side::Sell, 102, 4, 4, 5, OrderStatus::Open));
    });

    Client client(4096);
    client.connect("127.0.0.1", testServer.port());
    const nlohmann::json response = client.request(nlohmann::json::parse(R"({"type":"PRINT"})"));

    EXPECT_EQ(response.at("status"), "OK");
    const auto& buy = response.at("result").at("buy");
    const auto& sell = response.at("result").at("sell");

    // buy: сначала более высокая цена (101 -> id 3), затем уровень 100 в
    // порядке поступления (id 1, затем id 2).
    ASSERT_EQ(buy.size(), 3u);
    EXPECT_EQ(buy[0].at("order_id"), 3);
    EXPECT_EQ(buy[1].at("order_id"), 1);
    EXPECT_EQ(buy[2].at("order_id"), 2);

    // sell: по возрастанию цены — 102 (id 5), затем 105 (id 4).
    ASSERT_EQ(sell.size(), 2u);
    EXPECT_EQ(sell[0].at("order_id"), 5);
    EXPECT_EQ(sell[1].at("order_id"), 4);
}

// Критерий 8: PRINT показывает остаток частично исполненной заявки
// (remaining_quantity), а не исходный объём (initial_quantity).
TEST(NetworkTest, PrintShowsRemainingQuantityNotInitial) {
    TestServer testServer(4096);

    runInIoContext(testServer, [&testServer] {
        testServer.processor.restoreOrder(std::make_shared<Order>(
            7, Side::Buy, 100, 4, 10, 1, OrderStatus::PartiallyFilled));
    });

    Client client(4096);
    client.connect("127.0.0.1", testServer.port());
    const nlohmann::json response = client.request(nlohmann::json::parse(R"({"type":"PRINT"})"));

    ASSERT_EQ(response.at("result").at("buy").size(), 1u);
    const auto& entry = response.at("result").at("buy")[0];
    EXPECT_EQ(entry.at("order_id"), 7);
    EXPECT_EQ(entry.at("price"), 100);
    EXPECT_EQ(entry.at("quantity"), 4);
}

// Критерий 9: одна команда с одним command_id отправлена дважды — второй
// ответ совпадает с первым целиком (включая trades), а число сделок в БД
// не выросло (REQ-API-09, "ловушка идемпотентного ответа").
TEST(NetworkTest, IdempotentAddOverSocketReturnsSameResponseAndDoesNotDuplicateTrade) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    nlohmann::json first;
    nlohmann::json second;
    {
        // Область видимости обязана закончиться (и присоединить io-поток
        // сервера) раньше, чем тест снова тронет *connOpt напрямую: та же
        // PgConnection передана RequestRouter и всё это время может
        // использоваться io-потоком сервера, а libpq запрещает работу с
        // одним соединением из двух потоков.
        TestServer testServer(4096, &*connOpt);
        Client client(4096);
        client.connect("127.0.0.1", testServer.port());

        nlohmann::json buy;
        buy["type"] = "ADD";
        buy["order_id"] = 1;
        buy["side"] = "BUY";
        buy["price"] = 100;
        buy["quantity"] = 10;
        buy["command_id"] = "cmd-idem-buy";
        ASSERT_EQ(client.request(buy).at("status"), "OK");

        nlohmann::json sell;
        sell["type"] = "ADD";
        sell["order_id"] = 2;
        sell["side"] = "SELL";
        sell["price"] = 100;
        sell["quantity"] = 4;
        sell["command_id"] = "cmd-idem-sell";

        first = client.request(sell);
        ASSERT_EQ(first.at("status"), "OK");
        ASSERT_EQ(first.at("trades").size(), 1u);

        second = client.request(sell);
    }
    EXPECT_EQ(second, first);

    PgResult tradeCount = connOpt->execute(
        "SELECT COUNT(*) FROM trades WHERE buy_order_id = $1 AND sell_order_id = $2",
        {std::optional<std::string>("1"), std::optional<std::string>("2")});
    ASSERT_EQ(tradeCount.rowCount(), 1);
    EXPECT_EQ(tradeCount.getValue(0, 0), "1");
}

// Ответ MESSAGE_TOO_LARGE обязан встать в очередь записи вслед за уже
// отправляемым (и ещё не завершённым) ответом на PRINT, а не оборвать
// соединение раньше, чем этот ответ дошёл до клиента. Книга наполняется
// напрямую через restoreOrder (без сети и без БД, docs/task4/
// 02-network-protocol.md, раздел 5.3 про очередь записи) — так ответ на
// PRINT весит сотни килобайт, а async_write не вызывает свой обработчик из
// инициирующего вызова: очередь в Session ставится именно потому, что
// порядок завершения асинхронных операций определяется циклом событий, а
// не порядком вызовов enqueueResponse (как это было бы, если бы весь ответ
// укладывался в один системный вызов записи, как маленький PONG).
TEST(NetworkTest, MessageTooLargeIsQueuedBehindLargePrintResponse) {
    // Больше самого большого ответа на PRINT ниже (около 700-900 КБ) — сам
    // PRINT не должен получить MESSAGE_TOO_LARGE, только второй, заведомо
    // слишком большой кадр.
    constexpr std::size_t kServerLimit = 1500000;
    TestServer testServer(kServerLimit);

    constexpr int kOrderCount = 15000;
    runInIoContext(testServer, [&testServer] {
        for (int i = 0; i < kOrderCount; ++i) {
            testServer.processor.restoreOrder(std::make_shared<Order>(
                i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
        }
    });

    // Таймаут поднят с умолчания в 2 секунды: около 700 КБ на цикл событий
    // под ThreadSanitizer (задача 12) заметно медленнее, чем на обычной
    // сборке, и дефолтный таймаут клиента стал бы источником плавающих
    // падений, не связанных с самим протоколом.
    Client client(kServerLimit * 2, std::chrono::seconds(10));
    client.connect("127.0.0.1", testServer.port());

    const std::string printFrame = client.encodeFrame(R"({"type":"PRINT"})");

    nlohmann::json bigRequest;
    bigRequest["type"] = "PING";
    bigRequest["padding"] = std::string(kServerLimit + 1000, 'x');
    const std::string oversizedFrame = client.encodeFrame(bigRequest.dump());

    // decodeHeader() отвергает кадр по первым четырём байтам заголовка, не
    // читая тело вовсе (docs/task4/02-network-protocol.md, раздел 1.1) —
    // поэтому вслед за запросом PRINT достаточно отправить только заголовок
    // второго кадра, срез уже закодированных байт (тот же приём, что и в
    // тесте фрагментации выше), а не мегабайт тела целиком. Отправка всего
    // тела заставила бы клиент и сервер одновременно писать друг другу
    // сотни килобайт, ничего не читая, — взаимная блокировка сокетов по
    // TCP-flow-control, а не то, что проверяет этот тест.
    const std::string oversizedFrameHeader = oversizedFrame.substr(0, kFrameHeaderSize);

    // Обе записи одним вызовом: сервер ещё пишет (заведомо большой) ответ
    // на PRINT, когда должен разобрать заголовок второго кадра и поставить
    // MESSAGE_TOO_LARGE в очередь вслед за уже начатой записью.
    client.sendRawBytes(printFrame + oversizedFrameHeader);

    const nlohmann::json printResponse = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(printResponse.at("status"), "OK");
    EXPECT_EQ(printResponse.at("result").at("buy").size(),
        static_cast<std::size_t>(kOrderCount));

    const nlohmann::json errorResponse = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(errorResponse.at("status"), "ERROR");
    EXPECT_EQ(errorResponse.at("error"), "MESSAGE_TOO_LARGE");

    // Обрыв различается от таймаута по типу исключения — тем же способом,
    // что и в тестах превышения размера выше.
    try {
        client.receiveFrame();
        FAIL() << "сервер обязан был закрыть соединение";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение осталось открытым: сервер молчит вместо закрытия";
    } catch (const NetworkError&) {
        SUCCEED();
    }
}

// Ответ, который сам
// (а не заявленный клиентом размер запроса) не помещается в
// max_message_size, обязан дойти до клиента диагностикой через ту же
// очередь записи, а не оборвать соединение без единого байта. Раздел 4
// контракта разрешает закрывать соединение молча только при нарушении
// протокола — здесь же запрос (PRINT) был совершенно корректным, соединение
// закрывает уже сам сервер, не сумевший закодировать честно построенный
// ответ.
TEST(NetworkTest, OversizedResponseIsReportedNotSilentlyDropped) {
    constexpr std::size_t kServerLimit = 4096;
    TestServer testServer(kServerLimit);

    // Книга, чей PRINT-ответ заведомо больше kServerLimit (см. runInIoContext
    // выше — восстановление заявок идёт в io-потоке сервера, а не из потока
    // теста).
    constexpr int kOrderCount = 500;
    runInIoContext(testServer, [&testServer] {
        for (int i = 0; i < kOrderCount; ++i) {
            testServer.processor.restoreOrder(std::make_shared<Order>(
                i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
        }
    });

    Client client(kServerLimit * 10);
    client.connect("127.0.0.1", testServer.port());
    client.sendRawBytes(client.encodeFrame(R"({"type":"PRINT"})"));

    const nlohmann::json response = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(response.at("status"), "ERROR");
    ASSERT_TRUE(response.contains("error"));
    ASSERT_TRUE(response.contains("message"));

    // Обрыв различается от таймаута по типу исключения — тем же способом,
    // что и в тестах превышения размера выше.
    try {
        client.receiveFrame();
        FAIL() << "сервер обязан был закрыть соединение";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение осталось открытым: сервер молчит вместо закрытия";
    } catch (const NetworkError&) {
        SUCCEED();
    }

    // Сервер остался жив и обслуживает следующего клиента — это не тот
    // фатальный сбой, что при PersistenceError, а несовпадение размера
    // ответа с конфигурацией.
    Client another(1024);
    another.connect("127.0.0.1", testServer.port());
    const nlohmann::json pong = another.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pong.at("status"), "OK");
    EXPECT_EQ(pong.at("result"), "PONG");
}

// Механизм фатального завершения после сбоя сохранения в БД
// (Session::fatalAfterWrite_ -> Server::onFatalShutdown_ ->
// Server::hadFatalError()) не был покрыт ни одним тестом. Сбой сохранения
// воспроизводится детерминированно и без порчи схемы или живого
// соединения: строка с тем же command_id заранее вставляется в
// processed_commands напрямую по SQL, минуя in-memory кеш идемпотентности
// свежего CommandProcessor — движок честно выполняет ADD (книга в памяти
// меняется), а запись результата упирается в нарушение уникальности
// command_id и оборачивается в PersistenceError тем же путём
// (persistence_service.cpp), что и настоящий сбой соединения с БД.
TEST(NetworkTest, PersistenceFailureSendsResponseThenClosesThenSetsFatalFlag) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    const std::string commandId = "cmd-fatal-read";
    connOpt->execute(
        "INSERT INTO processed_commands (command_id, command_type, status, result) "
        "VALUES ($1, $2, $3, $4)",
        {std::optional<std::string>(commandId), std::optional<std::string>("ADD"),
            std::optional<std::string>("OK"), std::nullopt});

    TestServer testServer(4096, &*connOpt);

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = 501;
    add["side"] = "BUY";
    add["price"] = 10;
    add["quantity"] = 1;
    add["command_id"] = commandId;

    Client client(4096);
    client.connect("127.0.0.1", testServer.port());
    const nlohmann::json response = client.request(add);
    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INTERNAL_ERROR");

    // Ответ дошёл целиком (иначе client.request() выше бросил бы) — теперь
    // соединение обязано быть закрыто сервером.
    try {
        client.receiveFrame();
        FAIL() << "сервер обязан был закрыть соединение после INTERNAL_ERROR";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение осталось открытым: сервер молчит вместо закрытия";
    } catch (const NetworkError&) {
        SUCCEED();
    }

    // io-поток сервера сам остановил себя изнутри (onFatalShutdown_):
    // join() здесь, отдельно от деструктора TestServer, — единственный
    // способ безопасно прочитать hadFatalError() без гонки, а деструктор,
    // видя ioThread уже неприсоединяемым, не станет join()'ить второй раз.
    joinIoThreadWithTimeout(testServer, std::chrono::seconds(5));
    EXPECT_TRUE(testServer.server.hadFatalError());
}

// Признак фатальной ошибки обязан остаться взведённым, даже если клиент
// отключился раньше, чем прочитал ответ, — в частности на ветке
// writeNext(), где async_write завершается ошибкой: onFatalShutdown_
// обязан вызываться и там, а не только на успешном пути записи.
// SO_LINGER{true, 0} форсирует немедленный RST вместо штатного FIN при
// закрытии клиентского сокета: иначе провал записи на сервере проявился бы
// не сразу, а после таймаута TCP, и тест не мог бы гарантированно уложиться
// в отведённое время.
//
// Момент разрыва сделан детерминированным полным циклом PING/PONG перед
// отправкой фатальной команды по тому же сокету: он доказывает, что
// сессия уже принята сервером и находится в состоянии чтения следующего
// кадра (readHeader), а не где-то в процессе accept, — без этого RST мог
// бы прийти раньше, чем сервер вообще успел прочитать тело ADD, команда не
// выполнилась бы вовсе, признак не взвёлся бы, и тест падал бы по
// сторожевому таймеру joinIoThreadWithTimeout на полностью исправном коде.
// Дальше тест намеренно допускает оба исхода: сервер мог успеть поставить
// ответ в буфер ядра до разрыва (тогда onFatalShutdown_ вызывается из ветки
// успешной записи в writeNext()) или получить ошибку записи из-за RST
// (веткой ошибки, ради которой тест писался) — с внешней стороны сокета
// оба неотличимы, а проверяется только то, что признак фатальности
// взведён в обоих случаях.
TEST(NetworkTest, PersistenceFailureKeepsFatalFlagWhenClientDisconnectsWithoutReading) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    const std::string commandId = "cmd-fatal-disconnect";
    connOpt->execute(
        "INSERT INTO processed_commands (command_id, command_type, status, result) "
        "VALUES ($1, $2, $3, $4)",
        {std::optional<std::string>(commandId), std::optional<std::string>("ADD"),
            std::optional<std::string>("OK"), std::nullopt});

    TestServer testServer(4096, &*connOpt);

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = 502;
    add["side"] = "BUY";
    add["price"] = 10;
    add["quantity"] = 1;
    add["command_id"] = commandId;

    const MessageCodec codec(4096);
    const std::string pingFrame = codec.encode(R"({"type":"PING"})");
    const std::string addFrame = codec.encode(add.dump());

    {
        // Сырой синхронный сокет вместо Client: тесту нужен полный контроль
        // над закрытием (SO_LINGER), которого у Client нет и не должно
        // быть — это специфика ровно одного сценария, а не часть
        // протокольного клиента.
        boost::asio::io_context rawIoContext;
        boost::asio::ip::tcp::socket rawSocket(rawIoContext);
        rawSocket.connect(boost::asio::ip::tcp::endpoint(
            boost::asio::ip::make_address("127.0.0.1"), testServer.port()));

        boost::asio::write(rawSocket, boost::asio::buffer(pingFrame));
        std::array<char, kFrameHeaderSize> pongHeader{};
        boost::asio::read(rawSocket, boost::asio::buffer(pongHeader));
        const std::uint32_t pongBodySize = codec.decodeHeader(pongHeader);
        std::string pongBody(pongBodySize, '\0');
        boost::asio::read(rawSocket, boost::asio::buffer(pongBody));

        boost::asio::write(rawSocket, boost::asio::buffer(addFrame));
        rawSocket.set_option(boost::asio::socket_base::linger(true, 0));
        rawSocket.close();
    }

    joinIoThreadWithTimeout(testServer, std::chrono::seconds(5));
    EXPECT_TRUE(testServer.server.hadFatalError());
}
