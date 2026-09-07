#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "client.hpp"
#include "config.hpp"
#include "exceptions.hpp"
#include "request_router.hpp"
#include "server.hpp"

using namespace matching_engine;

namespace {

// Обёртка "сервер, поднятый в этом же процессе, в отдельном потоке
// io_context" (обязательное условие ТЗ — иначе до сервера не добраться ни
// тесту через реальный сокет, ни в будущем ThreadSanitizer). port = 0 —
// операционная система выбирает порт сама, реальный номер известен сразу
// после конструктора Server (bind/listen там синхронны), поэтому никакой
// синхронизации ожидания готовности не нужно.
struct TestServer {
    explicit TestServer(std::size_t maxMessageSize)
        : config{"127.0.0.1", 0, maxMessageSize},
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
        // Прямой вызов метода остановки (docs/hw4/tasks/task-04.md,
        // "Границы": "остановка сервера в тестах выполняется прямым
        // вызовом метода остановки"), а не только обрыв io_context.
        server.stop();
        // io_context::stop() безопасен для вызова из чужого потока по
        // контракту Asio — здесь вызывается из потока теста, пока
        // io_context крутится в ioThread.
        ioContext.stop();
        ioThread.join();
    }

    unsigned short port() const { return server.port(); }

    // Порядок полей значим: server хранит ссылку на router и работает
    // через ioContext, поэтому обязан разрушаться раньше них обоих, а
    // деструкторы полей вызываются в порядке, обратном объявлению.
    RequestRouter router;
    boost::asio::io_context ioContext;
    ServerConfig config;
    Server server;
    std::thread ioThread;
};

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

// "Что сделать", пункт 4: любой тип, кроме PING (в том числе команды
// предметной области — они появятся только в задаче 05), получает такой
// же ответ, как и по-настоящему неизвестная команда.
TEST(NetworkTest, UnknownCommandTypeReturnsInvalidRequest) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());
    const nlohmann::json response = client.request(nlohmann::json::parse(R"({"type":"ADD"})"));

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

// Второй круг ревью задачи 04: MESSAGE_TOO_LARGE обязан встать в очередь
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

// Прямой вызов Server::stop() (раздел "Границы" задачи 04): закрывает
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
