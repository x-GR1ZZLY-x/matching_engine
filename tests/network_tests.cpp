#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <memory>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <csignal>
#include <optional>
#include <pthread.h>

#include "client.hpp"
#include "command.hpp"
#include "command_processor.hpp"
#include "config.hpp"
#include "exceptions.hpp"
#include "execution_result.hpp"
#include "message_codec.hpp"
#include "order.hpp"
#include "order_repository.hpp"
#include "pg_connection.hpp"
#include "pg_result.hpp"
#include "recovery_service.hpp"
#include "request_router.hpp"
#include "server.hpp"
#include "signal_handler.hpp"
#include "test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

namespace {

// Восстанавливает маску сигналов процесса на выходе из области видимости —
// тот же приём и то же обоснование, что и в tests/signal_handler_tests.cpp:
// SignalHandler::blockSignals() меняет маску вызывающего потока, а маска —
// свойство процесса, и тест обязан вернуть её в исходное состояние, иначе
// SIGTERM, посланный этим тестом, ушёл бы дальше в соседние тесты того же
// бинарника.
class SignalMaskGuard {
public:
    SignalMaskGuard() {
        pthread_sigmask(SIG_SETMASK, nullptr, &original_);
    }
    ~SignalMaskGuard() {
        pthread_sigmask(SIG_SETMASK, &original_, nullptr);
    }

private:
    sigset_t original_{};
};

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
    // readTimeout передаётся дальше в Server как есть — по умолчанию 60
    // секунд (тот же дефолт, что и у Server), тесты таймаута чтения
    // передают заведомо малое значение явно.
    explicit TestServer(std::size_t maxMessageSize, PgConnection* connection = nullptr,
        std::chrono::seconds readTimeout = std::chrono::seconds(60))
        : router(processor, connection),
          config{"127.0.0.1", 0, maxMessageSize},
          server(ioContext, config, router, std::chrono::seconds(10), readTimeout) {
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

// То же назначение, что и у TestServer выше — сервер и его io-поток,
// поднятые в этом же процессе на порту 0, — но здесь CommandProcessor и
// RequestRouter собираются снаружи и передаются готовыми: это нужно тесту
// восстановления через перезапуск, где processor обязан пройти
// recoverState() до того, как Server начнёт его обслуживать (REQ-NET-14),
// а не быть свежесозданным изнутри обёртки, как в TestServer.
//
// Деструктор не сворачивает сервер штатно: server.stop() лишь ставит задачу
// остановки через post(), а следующая же строка, ioContext.stop(), обычно
// прерывает цикл событий раньше, чем эта задача успевает выполниться, —
// поэтому полагаться здесь на аккуратное закрытие сессий изнутри io_context
// нельзя. Освобождение происходит через деструкторы: acceptor закрывается
// вместе с Server, а сессии — вместе с io_context, когда тот отбрасывает свои
// незавершённые обработчики. Что деструктор действительно гарантирует — это
// ioThread.join() при любом пути выхода из области видимости, включая
// досрочный возврат из ASSERT_*: без него тест, упавший на промежуточной
// проверке до явной остановки, уносил бы весь процесс в std::terminate()
// из-за неприсоединённого std::thread вместо понятного красного результата.
struct ManualServer {
    ManualServer(RequestRouter& router, std::size_t maxMessageSize)
        : config{"127.0.0.1", 0, maxMessageSize}, server(ioContext, config, router) {
        server.start();
        ioThread = std::thread([this] {
            try {
                ioContext.run();
            } catch (const std::exception& e) {
                ADD_FAILURE() << "io_context::run() threw: " << e.what();
            }
        });
    }

    ~ManualServer() {
        server.stop();
        ioContext.stop();
        if (ioThread.joinable()) {
            ioThread.join();
        }
    }

    unsigned short port() const { return server.port(); }

    // Порядок полей значим, как и в TestServer выше: server хранит ссылку на
    // router и работает через ioContext, поэтому обязан разрушаться раньше
    // ioContext (деструкторы полей вызываются в порядке, обратном объявлению
    // — отсюда ioContext объявлен первым). router передаётся снаружи по
    // ссылке и не хранится этой обёрткой — время его жизни обеспечивает
    // вызывающий: router (а вместе с ним и CommandProcessor, на который он
    // ссылается) обязан пережить ManualServer целиком.
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
// REQ-TEST-11: несколько тестов ниже читают ответ сервера
// напрямую через синхронный boost::asio::read на сыром сокете (им нужен
// контроль над байтами, которого нет у Client) — а у такого чтения, в
// отличие от Client (см. src/client.cpp, каждая операция которого ограничена
// io_context::run_for(timeout)), по умолчанию нет предела по времени вообще.
// При неответившем сервере (регрессия, из-за которой ответ не пришёл бы)
// это было бы зависанием, а не падением, — то, что явно запрещено ТЗ. Работа
// выполняется во вспомогательном потоке; исключение из неё пробрасывается в
// поток теста через future.get(), чтобы обычная сетевая ошибка (например,
// обрыв соединения) осталась падением через тот же путь, что и раньше, а не
// стала std::terminate() в чужом потоке. При исчерпании таймаута тест
// фиксирует падение и завершает процесс принудительно — тем же приёмом, что
// и остальные ограничения по времени в этом файле (gtest_discover_tests
// запускает каждый тест отдельным процессом, поэтому аварийный выход уносит
// только его).
void readExactWithDeadline(boost::asio::ip::tcp::socket& socket,
    boost::asio::mutable_buffer buffer, std::chrono::seconds timeout) {
    std::promise<void> done;
    std::future<void> doneFuture = done.get_future();
    std::thread reader([&socket, buffer, &done] {
        try {
            boost::asio::read(socket, buffer);
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });

    if (doneFuture.wait_for(timeout) != std::future_status::ready) {
        ADD_FAILURE() << "сервер не ответил за отведённое время";
        // std::_Exit не сбрасывает буферы stdio; под ctest stdout полностью
        // буферизован, и без явного fflush() оператор получил бы код 1 и
        // пустой вывод — ни имени теста, ни сообщения ADD_FAILURE выше.
        std::fflush(nullptr);
        std::_Exit(1);
    }
    reader.join();
    doneFuture.get();
}

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

// Пауза заданной длительности без std::this_thread::sleep_for (запрещён
// правилами сетевых тестов): ожидание на future, которое никто не наполняет,
// — тот же приём, разрешённый явно ("только promise/future, condition_variable,
// callback или completion handler"), которым остальные помощники этого файла
// дожидаются готовности асинхронной работы или её отсутствия. Здесь future
// никогда не станет готовым, поэтому wait_for детерминированно блокируется на
// duration и возвращается по истечении срока. Нужна тестам, которые по своей
// природе проверяют исход гонки с настоящим таймером чтения, а не наблюдаемое
// событие без него. Таких тестов ниже два, и в обоих клиент обязан оставаться
// неподвижным дольше readTimeout_ — наблюдать это как событие нельзя, потому
// что единственное наблюдаемое событие (закрытие сокета) видно лишь при
// чтении, а чтение разблокировало бы застрявшую запись и разрушило сам
// сценарий: ReadTimeoutClosesSessionEvenWhileResponseNeverDrains доказывает,
// что таймаут закрывает соединение даже с непустой очередью записи, а
// StopCancelsStaleReadTimeoutWhileDrainingQueuedResponse — что устаревший
// таймер не оборвал ещё не законченную отправку ответа. Замечание ревью:
// раньше этим же приёмом просто вставлялась пауза между раундами теста
// FrequentCommandsResetReadTimeoutAndSessionStaysOpen — там
// она была неотличима от sleep_for по сути и заменена доказательством через
// суммарное время жизни соединения без всякой паузы.
void waitBriefly(std::chrono::milliseconds duration) {
    std::promise<void> neverFulfilled;
    neverFulfilled.get_future().wait_for(duration);
}

}

// PING без обращения к разбору команд предметной области.
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

// Замечание ревью второго круга, п.1 — живой сценарий, воспроизведённый на
// работающем сервере: запрос без поля "type", но со сверхдлинным
// command_id, эхо которого само по себе превысило бы max_message_size. До
// исправления порядка проверок в RequestRouter::handle такой запрос
// проходил проверку "это объект со строковым type" (валится на "type") и
// уходил в ответ с неотфильтрованным эхом command_id раньше проверки его
// длины — Session::enqueueResponse бросал MessageTooLargeError, попытка
// построить RESPONSE_TOO_LARGE с тем же неотфильтрованным эхом бросала
// снова, и соединение закрывалось без единого байта ответа (клиент получал
// голый обрыв, в логе сервера — ни строки).
TEST(NetworkTest, RequestWithoutTypeAndOversizedCommandIdGetsResponseConnectionStaysAlive) {
    constexpr std::size_t kServerLimit = 4096;
    TestServer testServer(kServerLimit);

    Client client(kServerLimit * 4);
    client.connect("127.0.0.1", testServer.port());

    // Длина command_id подобрана так, чтобы сам запрос ({"command_id":"..."},
    // 17 байт обрамления + N) укладывался в kServerLimit, а неотфильтрованное
    // эхо в ответе об ошибке (131 байт фиксированной части ответа + N) —
    // нет: N=4079 даёт запрос ровно 4096 байт (помещается) и гипотетический
    // ответ 4210 байт (не поместился бы). Это и воспроизводит живой
    // сценарий: кадр запроса читается целиком, а не отвергается на
    // заголовке (тем MESSAGE_TOO_LARGE, который проверяется в других
    // тестах) — отказать здесь обязан именно фильтр длины command_id.
    constexpr std::size_t kCommandIdLength = 4079;
    nlohmann::json request;
    request["command_id"] = std::string(kCommandIdLength, 'x');
    client.sendRawBytes(client.encodeFrame(request.dump()));

    const nlohmann::json response = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    // Неправдоподобно длинный command_id не эхируется вовсе.
    EXPECT_FALSE(response.contains("command_id"));

    // Соединение осталось живым: следующая команда в том же соединении
    // выполняется успешно.
    const nlohmann::json pong = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pong.at("status"), "OK");
    EXPECT_EQ(pong.at("result"), "PONG");
}

// Замечание ревью второго круга, п.3: сетевого теста на то, что многобайтовый
// неизвестный тип не рвёт соединение, не было — а это ровно тот симптом,
// который наблюдался вживую (json::type_error из dump() на невалидном
// UTF-8-хвосте, вылетающий из RequestRouter::handle в Session, которая
// закрывает соединение catch(const std::exception&)). "€" повторён 30 раз
// (90 байт) — та же математика границы, что и в
// RequestRouterTest.ImplausiblyLongMultibyteTypeIsTruncatedOnCodepointBoundary,
// но здесь проверяется весь сетевой путь, а не только RequestRouter.
TEST(NetworkTest, MultibyteUnknownTypeReturnsErrorThenNextCommandSucceeds) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    std::string hugeType;
    for (int i = 0; i < 30; ++i) {
        hugeType += "\xE2\x82\xAC";
    }
    nlohmann::json request;
    request["type"] = hugeType;

    const nlohmann::json response = client.request(request);
    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");

    // Соединение осталось живым — следующая команда в том же соединении
    // выполняется успешно.
    const nlohmann::json pingResponse =
        client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pingResponse.at("status"), "OK");
    EXPECT_EQ(pingResponse.at("result"), "PONG");
}

// Кадр отправлен четырьмя частями — часть заголовка, остаток
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

// Три кадра одним вызовом записи, три ответа в том же
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

// Заголовок объявляет размер больше server.max_message_size.
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

// Битый JSON не разрывает соединение — следующая команда в
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

// Клиент отключился, не дочитав ответ, — сервер не падает,
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

// Прямой вызов Server::stop() (REQ-NET-08, docs/task4/
// 01-service-lifecycle.md, раздел 4.1): закрывает acceptor — новые
// подключения больше не принимаются, — и обходит реестр живых сессий,
// прося каждую закрыться (шаг 5 раздела 4.1, REQ-EXT-08). Раньше stop()
// трогал только acceptor, и существующая сессия продолжала
// работать; с обходом реестра это поведение стало намеренно другим — сама
// суть graceful drain в том, что stop() закрывает и уже принятые
// соединения, а не только вход для новых. У сессии из этого теста очередь
// записи пуста в момент остановки (последний обмен уже завершён), поэтому
// beginClose() закрывает её сокет немедленно; поведение с непустой
// очередью (ответ дописывается до конца перед закрытием) отдельно проверяет
// NetworkTest.StopSendsQueuedResponseBeforeClosingSocket.
TEST(NetworkTest, StopClosesAcceptorAndDrainsExistingSession) {
    TestServer testServer(1024);

    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    // Первый обмен подтверждает, что соединение уже полностью принято
    // сервером (async_accept отработал, Session::start() запущен) и что
    // сессия успела вернуться к чтению следующего кадра с пустой очередью
    // записи до того, как сервер будет остановлен.
    const nlohmann::json warmup = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    ASSERT_EQ(warmup.at("status"), "OK");

    testServer.server.stop();

    // stop() лишь ставит задачу через post — сам по себе он не гарантирует,
    // что сетевой поток успел её обработать. Маркер, протолкнутый через тот
    // же io_context следом, доказывает это: обработчики выполняются в
    // порядке очереди, поэтому возврат runInIoContext означает, что
    // обработчик stop() уже отработал. Без этого тест зависел бы от гонки —
    // на медленном сетевом потоке он падал бы и на исправном коде.
    runInIoContext(testServer, [] {});

    // Сессия закрыта остановкой сервера — следующий запрос на этом же
    // соединении обязан провалиться обрывом, а не получить ответ и не
    // повиснуть до таймаута.
    try {
        client.request(nlohmann::json::parse(R"({"type":"PING"})"));
        FAIL() << "сессия обязана была закрыться при остановке сервера";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение осталось открытым: остановка не закрыла существующую сессию";
    } catch (const NetworkError&) {
        SUCCEED();
    }

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

// ADD BUY 100x10, затем ADD SELL 100x4 — ответ на вторую
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

// Пример запроса из текста задания, вставленный дословно (поле
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

// Поле "id" вместо "order_id" — синоним из консольного режима
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

// CANCEL несуществующей заявки — status ERROR, error
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

// После ответа об ошибке в том же соединении следующая
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

// MODIFY и рыночная заявка (order_type: MARKET) работают через
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

// PRINT возвращает книгу в формате раздела 3.4 контракта, и
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

// PRINT показывает остаток частично исполненной заявки
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

// Одна команда с одним command_id отправлена дважды — второй
// ответ совпадает с первым целиком (включая trades), число сделок в БД
// не выросло (REQ-API-09, "ловушка идемпотентного ответа"), и — отдельно —
// повторная отправка не трогает книгу заявок ещё раз. Снимок PRINT снят до
// повтора и после: реализация, которая на повторе честно проводит команду
// через движок заново (а не просто отдаёт закешированный ответ), дала бы
// на второй PRINT другую книгу (сделка исполнилась бы дважды, BUY 1 остался
// бы без остатка), поэтому сравнения одних лишь ответов ADD недостаточно.
// Ожидаемое содержимое снимка (а не просто равенство "снимок1 == снимок2")
// нужно, чтобы не пропустить реализацию, у которой оба снимка совпадающе
// пусты или совпадающе неверны.
TEST(NetworkTest, IdempotentAddOverSocketReturnsSameResponseAndDoesNotDuplicateTrade) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    nlohmann::json first;
    nlohmann::json second;
    nlohmann::json bookBeforeRepeat;
    nlohmann::json bookAfterRepeat;
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

        const nlohmann::json printRequest = nlohmann::json::parse(R"({"type":"PRINT"})");
        bookBeforeRepeat = client.request(printRequest);
        ASSERT_EQ(bookBeforeRepeat.at("status"), "OK");

        second = client.request(sell);

        bookAfterRepeat = client.request(printRequest);
        ASSERT_EQ(bookAfterRepeat.at("status"), "OK");
    }
    EXPECT_EQ(second, first);
    EXPECT_EQ(bookAfterRepeat, bookBeforeRepeat);

    // Содержимое снимка: BUY 1 с остатком 6 (10 - 4 из единственной сделки),
    // SELL пуста — заявка 2 исполнилась целиком и с книги не осталась.
    const auto& buyBefore = bookBeforeRepeat.at("result").at("buy");
    ASSERT_EQ(buyBefore.size(), 1u);
    EXPECT_EQ(buyBefore[0].at("order_id"), 1);
    EXPECT_EQ(buyBefore[0].at("quantity"), 6);
    EXPECT_TRUE(bookBeforeRepeat.at("result").at("sell").empty());

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
    // под ThreadSanitizer заметно медленнее, чем на обычной
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

// Ответ, который сам (а не заявленный клиентом размер запроса) не
// помещается в max_message_size, обязан дойти до клиента диагностикой с
// кодом RESPONSE_TOO_LARGE через ту же очередь записи, а соединение — не
// закрываться (docs/task4/02-network-protocol.md, раздел 4.1). Запрос (PRINT) был совершенно корректным, кадр запроса
// прочитан целиком, позиция в потоке известна — следующая команда в этом же
// соединении обязана выполниться, а не наткнуться на разорванный сокет.
TEST(NetworkTest, OversizedResponseReturnsResponseTooLargeAndKeepsConnectionAlive) {
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
    EXPECT_EQ(response.at("error"), "RESPONSE_TOO_LARGE");
    ASSERT_TRUE(response.contains("message"));

    // Соединение осталось живым: следующая команда в том же соединении
    // (тот же client, тот же сокет) выполняется успешно — это и есть
    // проверка того, что соединение осталось рабочим, а не просто "сервер жив и принял кого-то ещё".
    const nlohmann::json pong = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pong.at("status"), "OK");
    EXPECT_EQ(pong.at("result"), "PONG");
}

// Замечание ревью, пункт 2: запрос, ответ на который не помещается в
// max_message_size, уже прочитан и выполнен целиком (не то же самое, что
// MESSAGE_TOO_LARGE у заголовка, где тела ещё не было) — раздел 3.5
// контракта требует эха command_id везде, где запрос удалось разобрать
// настолько, чтобы его извлечь, и раздел 4.1 исключения для этого случая не
// вводит.
TEST(NetworkTest, OversizedResponseEchoesCommandId) {
    constexpr std::size_t kServerLimit = 4096;
    TestServer testServer(kServerLimit);

    constexpr int kOrderCount = 500;
    runInIoContext(testServer, [&testServer] {
        for (int i = 0; i < kOrderCount; ++i) {
            testServer.processor.restoreOrder(std::make_shared<Order>(
                i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
        }
    });

    Client client(kServerLimit * 10);
    client.connect("127.0.0.1", testServer.port());
    client.sendRawBytes(
        client.encodeFrame(R"({"type":"PRINT","command_id":"cmd-print-too-large"})"));

    const nlohmann::json response = nlohmann::json::parse(client.receiveFrame());
    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "RESPONSE_TOO_LARGE");
    EXPECT_EQ(response.at("command_id"), "cmd-print-too-large");
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

    // REQ-COMPAT-05 через сетевой путь: движок в памяти честно сопоставил
    // ADD (книга изменилась), но вся транзакция сохранения (заявка, сделки,
    // processed_commands) обязана откатиться целиком из-за конфликта на
    // последнем шаге (INSERT processed_commands с уже занятым command_id,
    // PersistenceService::save, docs/task4/02-network-protocol.md, раздел
    // 3.5) — заявка не должна просочиться в БД частично, отдельно от
    // остального эффекта команды.
    PgResult orderRow = connOpt->execute(
        "SELECT COUNT(*) FROM orders WHERE order_id = $1", {std::optional<std::string>("501")});
    ASSERT_EQ(orderRow.rowCount(), 1);
    EXPECT_EQ(orderRow.getValue(0, 0), "0");
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
        readExactWithDeadline(rawSocket, boost::asio::buffer(pongHeader), std::chrono::seconds(5));
        const std::uint32_t pongBodySize = codec.decodeHeader(pongHeader);
        std::string pongBody(pongBodySize, '\0');
        readExactWithDeadline(rawSocket, boost::asio::buffer(pongBody), std::chrono::seconds(5));

        boost::asio::write(rawSocket, boost::asio::buffer(addFrame));
        rawSocket.set_option(boost::asio::socket_base::linger(true, 0));
        rawSocket.close();
    }

    joinIoThreadWithTimeout(testServer, std::chrono::seconds(5));
    EXPECT_TRUE(testServer.server.hadFatalError());
}

// Замечание ревью, пункт 8: из трёх сочетаний "ответ не поместился" и
// "сбой сохранения" тестом покрыто было только одно. Не проверялась именно
// та ветка, где closeAfterWrite_/fatalAfterWrite_ взводятся из-под catch
// (Session::readBody, вложенный catch(MessageTooLargeError&), строки,
// обрабатывающие сбой отправки даже короткого RESPONSE_TOO_LARGE) — а
// признак фатальности терялся на соседних ветках дважды за проект.
// max_message_size здесь подобран так, что входящий ADD (96 байт) проходит,
// а оба возможных ответа — INTERNAL_ERROR после PersistenceError (124
// байта) и сам RESPONSE_TOO_LARGE (131 байт) — не помещаются в лимит.
// Сервер не может отправить клиенту вообще ничего и обязан закрыть
// соединение без единого байта ответа, но всё равно взвести признак
// фатальности и остановить сервис — то же самое, что происходит, когда
// ответ всё-таки помещается, просто без промежуточного шага.
TEST(NetworkTest, PersistenceFailureWithUnfittableResponseStillSetsFatalFlag) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    const std::string commandId = "cmd-fatal-tiny";
    connOpt->execute(
        "INSERT INTO processed_commands (command_id, command_type, status, result) "
        "VALUES ($1, $2, $3, $4)",
        {std::optional<std::string>(commandId), std::optional<std::string>("ADD"),
            std::optional<std::string>("OK"), std::nullopt});

    constexpr std::size_t kTinyLimit = 100;
    TestServer testServer(kTinyLimit, &*connOpt);

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = 503;
    add["side"] = "BUY";
    add["price"] = 10;
    add["quantity"] = 1;
    add["command_id"] = commandId;

    // Клиентский кодек намеренно с бОльшим пределом, чем у сервера — иначе
    // encodeFrame отверг бы кадр сам, ещё до отправки.
    Client client(4096);
    client.connect("127.0.0.1", testServer.port());
    client.sendRawBytes(client.encodeFrame(add.dump()));

    try {
        client.receiveFrame();
        FAIL() << "сервер обязан был закрыть соединение, не отправив ответ";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение осталось открытым: сервер молчит вместо закрытия";
    } catch (const NetworkError&) {
        SUCCEED();
    }

    joinIoThreadWithTimeout(testServer, std::chrono::seconds(5));
    EXPECT_TRUE(testServer.server.hadFatalError());
}

// Подключение к порту, который никто не слушает: клиент обязан получить
// отказ немедленно и именно обрывом, а не молчанием до истечения таймаута.
// На этом держится поведение консольной программы (src/client_main.cpp):
// она сообщает человеку понятную причину и завершается с ненулевым кодом,
// а не подвисает на пустом порту.
TEST(NetworkTest, ConnectToClosedPortFailsWithConnectionErrorNotTimeout) {
    unsigned short closedPort = 0;
    {
        // Порт выбирает операционная система, а после разрушения сервера
        // его больше никто не слушает — это надёжнее произвольно взятого
        // номера, который на машине разработчика может оказаться занят.
        TestServer testServer(1024);
        closedPort = testServer.port();
    }

    Client client(1024);
    try {
        client.connect("127.0.0.1", closedPort);
        FAIL() << "подключение к закрытому порту обязано провалиться";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "отказ в подключении обязан приходить обрывом, а не таймаутом";
    } catch (const NetworkError&) {
        SUCCEED();
    }
}

// Graceful shutdown (docs/task4/01-service-lifecycle.md). Сервер поднят
// вручную (не через TestServer, чей деструктор форсирует
// io_context::stop() — здесь ровно это принудительное вмешательство и
// проверяется как отдельная, наблюдаемая величина, а не смешивается с
// обычной остановкой теста).

// Сервер поднят в этом же
// процессе, клиент подключён и выполнил полный обмен — сессия принята и
// висит на чтении следующего кадра. Остановка запускается тем же путём,
// каким её запускает сигнал (единственное, что делает сигнальный поток над
// состоянием сервера, — Server::stop(), раздел 3 документа), и вся
// последовательность завершения обязана уложиться в ограничение по
// времени: io_context.run() возвращается сама, естественным исчерпанием
// работы (шаг 7 раздела 4.1), а не по принудительному io_context::stop().
//
// Это обеспечено тем, что клиент действительно подключён и выполнил
// запрос: без этого остановка без единого соединения прошла бы успешно
// даже в реализации, забывшей обойти реестр сессий, и тест ничего не
// проверял бы.
//
// Инъекция — убрать обход реестра сессий из Server::stop()
// (цикл "for (const std::weak_ptr<Session>& weak : sessions_) ...") —
// заставляет этот тест падать по истечении ограничения времени, а не
// висеть, потому что при исчерпании времени тест завершает свой процесс
// принудительно (пункт 5 раздела 7.2 документа); gtest_discover_tests
// запускает каждый тест отдельным процессом, поэтому падает только он.
TEST(NetworkTest, StopDrainsHangingSessionWithoutDeadlock) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, 4096};
    boost::asio::io_context ioContext;
    Server server(ioContext, config, router);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    Client client(4096);
    client.connect("127.0.0.1", server.port());
    const nlohmann::json pong = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    ASSERT_EQ(pong.at("status"), "OK");

    // Сессия сейчас снова висит на чтении следующего кадра (readHeader()
    // после ответа на PING) — именно это незавершённое чтение обязана
    // закрыть остановка, чтобы io_context.run() смогла вернуться сама.
    server.stop();

    std::promise<void> stopped;
    std::future<void> stoppedFuture = stopped.get_future();
    std::thread joiner([&ioThread, &stopped] {
        ioThread.join();
        stopped.set_value();
    });

    constexpr std::chrono::seconds kTimeout(5);
    if (stoppedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась за отведённое время"
                          " — обход реестра сессий, вероятно, не выполнен";
        // Присоединять поток, застрявший в незавершившейся run(), нельзя —
        // это и есть зависание, от которого тест обязан отличаться падением
        // (раздел 7.2, пункт 5 документа), а не попыткой join() заблокиро-
        // ванного потока.
        std::fflush(nullptr);
        std::_Exit(1);
    }
    joiner.join();
}

// По истечении предельного времени
// остановки сетевой поток останавливает io_context принудительно —
// наблюдаемо через Server::wasForceStopped(). Малое shutdownTimeout
// передано явно через параметр конструктора Server, чтобы проверка не
// ждала полные десять секунд и вообще была осуществима за разумное время;
// значение по умолчанию документа при этом не меняется.
//
// Ответ на PRINT над книгой из kOrderCount заявок весит десятки мегабайт и
// физически не помещается в буферы TCP на localhost, если клиент вообще
// не читает, — запись остаётся незавершённой намного дольше отведённой
// секунды. Клиент читает только 4 байта заголовка кадра-ответа перед тем,
// как запустить остановку — этого достаточно, чтобы доказать, что сервер
// уже начал запись (Session::writeQueue_ не пуст), и застраховаться от
// гонки "остановка началась раньше, чем сессия вообще успела поставить
// ответ в очередь" (тогда закрытие пустой очереди прошло бы мгновенно, и
// принудительное завершение было бы не по адресу).
TEST(NetworkTest, StopForcesShutdownAfterDeadlineWhenWriteNeverDrains) {
    constexpr std::size_t kServerLimit = 32u * 1024u * 1024u;
    CommandProcessor processor;

    constexpr int kOrderCount = 100000;
    for (int i = 0; i < kOrderCount; ++i) {
        processor.restoreOrder(std::make_shared<Order>(
            i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
    }

    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, kServerLimit};
    boost::asio::io_context ioContext;
    constexpr std::chrono::seconds kShutdownTimeout(1);
    Server server(ioContext, config, router, kShutdownTimeout);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    boost::asio::io_context clientIoContext;
    boost::asio::ip::tcp::socket clientSocket(clientIoContext);
    clientSocket.connect(boost::asio::ip::tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), server.port()));

    const MessageCodec codec(kServerLimit);
    boost::asio::write(clientSocket, boost::asio::buffer(codec.encode(R"({"type":"PRINT"})")));

    std::array<char, kFrameHeaderSize> header{};
    readExactWithDeadline(clientSocket, boost::asio::buffer(header), std::chrono::seconds(5));

    // Дальше клиент намеренно не читает ничего — сессия остаётся с
    // недописанным ответом до самого конца теста.
    server.stop();

    std::promise<void> stopped;
    std::future<void> stoppedFuture = stopped.get_future();
    std::thread joiner([&ioThread, &stopped] {
        ioThread.join();
        stopped.set_value();
    });

    constexpr std::chrono::seconds kWait(10);
    if (stoppedFuture.wait_for(kWait) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась даже принудительно"
                          " по истечении shutdownTimeout";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    joiner.join();

    EXPECT_TRUE(server.wasForceStopped());

    boost::system::error_code ignored;
    clientSocket.close(ignored);
}

// Сессия с непустой очередью
// записи в момент остановки обязана отправить уже накопленный ответ до
// закрытия сокета, а не оборваться на середине. Тот же приём
// синхронизации, что и в тесте принудительного завершения выше: клиент
// читает только 4 байта заголовка, доказывая, что сервер уже начал запись,
// и лишь затем запускает остановку — иначе можно было бы случайно
// остановить сервер до того, как сессия вообще поставила ответ в очередь,
// и тест проверял бы пустую очередь, а не непустую.
TEST(NetworkTest, StopSendsQueuedResponseBeforeClosingSocket) {
    constexpr std::size_t kServerLimit = 2u * 1024u * 1024u;
    TestServer testServer(kServerLimit);

    constexpr int kOrderCount = 15000;
    runInIoContext(testServer, [&testServer] {
        for (int i = 0; i < kOrderCount; ++i) {
            testServer.processor.restoreOrder(std::make_shared<Order>(
                i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
        }
    });

    boost::asio::io_context rawIoContext;
    boost::asio::ip::tcp::socket rawSocket(rawIoContext);
    rawSocket.connect(boost::asio::ip::tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), testServer.port()));

    const MessageCodec codec(kServerLimit);
    boost::asio::write(rawSocket, boost::asio::buffer(codec.encode(R"({"type":"PRINT"})")));

    std::array<char, kFrameHeaderSize> header{};
    readExactWithDeadline(rawSocket, boost::asio::buffer(header), std::chrono::seconds(5));
    const std::uint32_t bodySize = codec.decodeHeader(header);

    // Очередь записи этой сессии на этот момент заведомо не пуста: доставлены
    // только четыре байта заголовка, а тело ответа — сотни килобайт.
    testServer.server.stop();

    // Дочитывание тела и проверка закрытия сокета — блокирующие
    // синхронные операции, которые не оборвать снаружи; правило ТЗ требует,
    // чтобы каждый сетевой тест падал, а не висел, если сервер не пришлёт
    // накопленный ответ (например, из-за регрессии, ломающей drain). Тот же
    // приём, что и в StopDrainsHangingSessionWithoutDeadlock: работа — во
    // вспомогательном потоке, ожидание — с ограничением по времени, при
    // исчерпании — падение и принудительный выход без попытки присоединить
    // застрявший поток.
    std::promise<void> done;
    std::future<void> doneFuture = done.get_future();
    std::thread reader([&rawSocket, bodySize, kOrderCount, &done] {
        std::string body(bodySize, '\0');
        boost::asio::read(rawSocket, boost::asio::buffer(body));
        const nlohmann::json response = nlohmann::json::parse(body);
        EXPECT_EQ(response.at("status"), "OK");
        EXPECT_EQ(response.at("result").at("buy").size(), static_cast<std::size_t>(kOrderCount));

        // Ответ доставлен целиком — сокет закрыт сервером сразу после него:
        // drain отработал по документу, а не оборвал соединение раньше
        // времени.
        char extra = 0;
        boost::system::error_code ec;
        const std::size_t transferred =
            boost::asio::read(rawSocket, boost::asio::buffer(&extra, 1), ec);
        EXPECT_EQ(transferred, 0u);
        EXPECT_TRUE(ec);
        done.set_value();
    });

    constexpr std::chrono::seconds kTimeout(10);
    if (doneFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "сервер не прислал накопленный ответ и не закрыл сокет"
                          " за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    reader.join();
}

// Session::readBody() безусловно вызывал readHeader() в конце, поэтому
// клиент, посылающий команды конвейером, не давал очереди записи опустеть —
// сессия жила до истечения предельного времени, и остановка становилась
// принудительной (REQ-THR-11, REQ-EXT-08). Тест воспроизводит этот сценарий:
// конвейер команд отправляется уже после stop(), пока сессия ещё дописывает
// предыдущий большой ответ. Ответ на PRINT над kOrderCount заявками (тот же
// размер книги, что и в StopForcesShutdownAfterDeadlineWhenWriteNeverDrains
// выше) весит несколько мегабайт и не помещается в буферы TCP на localhost,
// пока клиент его не читает, — это даёт заведомо широкое окно, в течение
// которого до фикса конвейер успел бы быть прочитан и обработан целиком.
TEST(NetworkTest, SessionDoesNotServePipelinedCommandsSentAfterStopBegins) {
    constexpr std::size_t kServerLimit = 32u * 1024u * 1024u;
    CommandProcessor processor;

    constexpr int kOrderCount = 100000;
    for (int i = 0; i < kOrderCount; ++i) {
        processor.restoreOrder(std::make_shared<Order>(
            i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
    }

    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, kServerLimit};
    boost::asio::io_context ioContext;
    Server server(ioContext, config, router);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    boost::asio::io_context clientIoContext;
    boost::asio::ip::tcp::socket clientSocket(clientIoContext);
    clientSocket.connect(boost::asio::ip::tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), server.port()));

    const MessageCodec codec(kServerLimit);
    boost::asio::write(clientSocket, boost::asio::buffer(codec.encode(R"({"type":"PRINT"})")));

    std::array<char, kFrameHeaderSize> header{};
    readExactWithDeadline(clientSocket, boost::asio::buffer(header), std::chrono::seconds(5));

    // Очередь записи этой сессии заведомо не пуста (доставлены только
    // четыре байта заголовка из нескольких мегабайт тела, а клиент дальше
    // пока ничего не читает) — beginClose() внутри stop() взводит
    // closeAfterWrite_ вместо немедленного закрытия сокета (раздел 4.1
    // документа, шаг 6). readHeader() для следующего кадра к этому моменту
    // уже вызван — readBody() запускает его сразу после обработки PRINT, не
    // дожидаясь окончания записи ответа, — это и есть единственное чтение,
    // уже стоявшее в очереди на момент решения об остановке.
    server.stop();

    // stop() лишь ставит задачу через post; без ожидания её обработки
    // следующий шаг (отправка конвейера) мог бы состояться до того, как
    // сетевой поток выполнит beginClose() и взведёт closeAfterWrite_, — и
    // тест падал бы на медленном сетевом потоке даже на исправном коде.
    // Обработчики io_context выполняются в порядке очереди, поэтому возврат
    // этого маркера доказывает, что обработчик stop() уже отработал (тот же
    // приём, что и runInIoContext, здесь — на локальном ioContext, к
    // которому у TestServer доступа нет).
    {
        std::promise<void> stopProcessed;
        std::future<void> stopProcessedFuture = stopProcessed.get_future();
        boost::asio::post(ioContext, [&stopProcessed] { stopProcessed.set_value(); });
        constexpr std::chrono::seconds kStopTimeout(5);
        if (stopProcessedFuture.wait_for(kStopTimeout) != std::future_status::ready) {
            ADD_FAILURE() << "остановка сервера не была обработана io_context за отведённое время";
            std::fflush(nullptr);
            std::_Exit(1);
        }
    }

    // Конвейер команд, отправленных после решения об остановке: до фикса
    // сессия обслужила бы их все одну за другой (данные уже лежат в
    // приёмном буфере сервера, а сам ответ на PRINT ещё не начал
    // опустошаться — клиент его не читает), не давая очереди записи
    // опустеть. Отправляем заведомо больше одной — фикс обязан ограничить
    // число обслуженных сверху единицей (тем чтением, что уже стояло в
    // очереди на момент stop()), а не оставить цикл безусловным.
    constexpr int kPipelinedCount = 20;
    for (int i = 0; i < kPipelinedCount; ++i) {
        boost::asio::write(clientSocket, boost::asio::buffer(codec.encode(R"({"type":"PING"})")));
    }

    // Только теперь начинаем читать оставшееся — до этого момента сокет
    // намеренно не читался дальше заголовка. Сервер намеренно не
    // дочитывает конвейер, отправленный после stop() (в этом и состоит
    // фикс), и закрывает сокет с непрочитанными байтами на своей приёмной
    // стороне; такое закрытие штатно даёт клиенту обрыв (RST), а не
    // аккуратный FIN, поэтому границы кадров после обрыва не
    // восстанавливаются и разбирать поток как последовательность кадров
    // нельзя. Считать сырые байты тоже бессмысленно: тело ответа на PRINT
    // занимает мегабайты и по шагу 6 раздела 4.1 документа обязано быть
    // дописано целиком, поэтому по их числу обслуженные PONG'и не отличить
    // от легитимного ответа. Показательно другое: сколько раз в потоке
    // встретилось слово PONG — снимок книги заявок его не содержит, а
    // каждый обслуженный PING даёт ровно одно вхождение.
    std::promise<std::string> receivedPromise;
    std::future<std::string> receivedFuture = receivedPromise.get_future();
    std::thread reader([&clientSocket, &receivedPromise] {
        std::vector<char> buffer(1u << 20);
        std::string received;
        boost::system::error_code ec;
        while (!ec) {
            const std::size_t transferred = clientSocket.read_some(boost::asio::buffer(buffer), ec);
            received.append(buffer.data(), transferred);
        }
        receivedPromise.set_value(std::move(received));
    });

    constexpr std::chrono::seconds kTimeout(10);
    if (receivedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "сокет не закрылся после конвейера команд, отправленного"
                          " после начала остановки";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    reader.join();

    std::promise<void> stopped;
    std::future<void> stoppedFuture = stopped.get_future();
    std::thread joiner([&ioThread, &stopped] {
        ioThread.join();
        stopped.set_value();
    });
    if (stoppedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    joiner.join();

    // До фикса сессия обслуживала бы весь конвейер: данные уже ждали в
    // приёмном буфере, и каждый следующий readHeader() вызывался
    // безусловно, — то есть в потоке нашлось бы kPipelinedCount вхождений
    // PONG. После фикса их не больше одного: единственное чтение, уже
    // стоявшее в очереди на момент stop() (REQ-THR-11, REQ-EXT-08). Ноль —
    // тоже корректный исход: то чтение могло не успеть получить кадр
    // целиком до закрытия сокета.
    //
    // Отдельно проверяется, что передача вообще состоялась: обрыв при
    // close() с непрочитанными байтами в приёмном буфере штатно может
    // забрать в RST хвост уже отправленного тела PRINT, поэтому точный
    // размер здесь не проверяется — только то, что соединение не оборвалось
    // мгновенно, не передав ничего.
    const std::string received = receivedFuture.get();
    std::size_t pongCount = 0;
    for (std::size_t pos = received.find("PONG"); pos != std::string::npos;
         pos = received.find("PONG", pos + 1)) {
        ++pongCount;
    }

    EXPECT_GE(received.size() + kFrameHeaderSize, std::size_t{4096})
        << "соединение оборвалось почти сразу, ничего толком не передав";
    EXPECT_LE(pongCount, std::size_t{1})
        << "обслужено " << pongCount << " команд конвейера из " << kPipelinedCount
        << ", а после начала остановки допустима максимум одна — та, чьё чтение"
           " уже стояло в очереди на момент stop()";

    boost::system::error_code ignored;
    clientSocket.close(ignored);
}

// Сценарий из docs/task4/02-network-protocol.md, раздел 4.1, подраздел
// "Изменяющая команда: команда выполнена, ответ не доставлен". Заявка,
// заведённая ниже, сопоставляется сразу против kSellOrderCount книжных
// заявок — ответ на неё несёт столько же сделок и заведомо не помещается в
// kServerLimit, а сам запрос (несколько десятков байт) — помещается
// свободно.
TEST(NetworkTest, ResponseTooLargeOnCompletedAddSaysCommandWasSaved) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    constexpr std::size_t kServerLimit = 4096;
    constexpr int kSellOrderCount = 200;

    {
        // Область видимости обязана закончиться (и присоединить io-поток
        // сервера) раньше прямого запроса к *connOpt ниже — то же
        // обязательство, что и в тесте сохранения ADD в базу
        // (AddOverSocketPersistsOrdersAndTradesToDatabase): libpq не
        // допускает работу с одним соединением из двух потоков одновременно.
        TestServer testServer(kServerLimit, &*connOpt);

        // sequenceNumber восстановленных заявок взят заведомо больше любого,
        // который SequenceGenerator (singleton, в этом тестовом процессе ещё
        // ни разу не вызывался) выдаст самой новой заявке ниже: restoreOrder
        // кладёт заявки в книгу мимо генератора и мимо БД, а sequence_number
        // в таблице orders уникален — коллизия с первым же вызовом
        // генератора (обычно 1) иначе оборвала бы сохранение уникальным
        // нарушением ключа.
        constexpr long long kRestoredSequenceBase = 1000000;
        runInIoContext(testServer, [&testServer] {
            for (int i = 0; i < kSellOrderCount; ++i) {
                testServer.processor.restoreOrder(std::make_shared<Order>(
                    i + 1, Side::Sell, 100, 1, 1, kRestoredSequenceBase + i, OrderStatus::Open));
            }
        });

        Client client(kServerLimit * 20);
        client.connect("127.0.0.1", testServer.port());

        nlohmann::json add;
        add["type"] = "ADD";
        add["order_id"] = 100001;
        add["side"] = "BUY";
        add["price"] = 100;
        add["quantity"] = kSellOrderCount;
        add["command_id"] = "cmd-response-too-large";

        const nlohmann::json response = client.request(add);
        EXPECT_EQ(response.at("status"), "ERROR");
        EXPECT_EQ(response.at("error"), "RESPONSE_TOO_LARGE");
        ASSERT_TRUE(response.at("message").is_string());
        const std::string message = response.at("message").get<std::string>();
        // Формулировка обязана исключать ложный вывод "заявка не принята":
        // должно быть явно сказано, что команда выполнена и сохранена.
        EXPECT_NE(message.find("executed"), std::string::npos) << message;
        EXPECT_NE(message.find("saved"), std::string::npos) << message;

        // Соединение осталось живым — ошибка пользователя его не разрывает.
        const nlohmann::json pong = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
        EXPECT_EQ(pong.at("status"), "OK");
    }

    // Команда действительно выполнена и сохранена, как и утверждает
    // сообщение: сделки в БД есть, несмотря на то что клиент их не увидел.
    PgResult tradeCount = connOpt->execute(
        "SELECT COUNT(*) FROM trades WHERE buy_order_id = $1", {std::optional<std::string>("100001")});
    ASSERT_EQ(tradeCount.rowCount(), 1);
    EXPECT_EQ(tradeCount.getValue(0, 0), std::to_string(kSellOrderCount));
}

// REQ-COMPAT-01: сохранение заявок, сделок и обработанных команд в
// PostgreSQL продолжает работать через сетевой путь. Проверяется прямым
// чтением таблиц orders, trades и processed_commands, а не только ответом
// клиенту — ответ мог бы быть верным и при отсутствии записи в базу
// (сериализация ответа и запись в БД — независимые шаги,
// docs/task4/02-network-protocol.md, раздел 3.7). processed_commands
// проверяется отдельно и по обоим command_id: это путь идемпотентности, а
// не побочный эффект записи orders/trades, и наличие результата (result не
// пуст) важно так же, как факт наличия самой строки — пустой result сделал
// бы повтор команды неотличимым от первого выполнения.
TEST(NetworkTest, AddOverSocketPersistsOrdersAndTradesToDatabase) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    {
        // Область видимости обязана закончиться (и присоединить io-поток
        // сервера) раньше прямых запросов к *connOpt ниже — то же
        // обязательство, что и в тесте идемпотентности выше: libpq не
        // допускает работу с одним соединением из двух потоков одновременно.
        TestServer testServer(4096, &*connOpt);
        Client client(4096);
        client.connect("127.0.0.1", testServer.port());

        nlohmann::json buy;
        buy["type"] = "ADD";
        buy["order_id"] = 701;
        buy["side"] = "BUY";
        buy["price"] = 100;
        buy["quantity"] = 10;
        buy["command_id"] = "cmd-persist-buy";
        ASSERT_EQ(client.request(buy).at("status"), "OK");

        nlohmann::json sell;
        sell["type"] = "ADD";
        sell["order_id"] = 702;
        sell["side"] = "SELL";
        sell["price"] = 100;
        sell["quantity"] = 4;
        sell["command_id"] = "cmd-persist-sell";
        ASSERT_EQ(client.request(sell).at("status"), "OK");
    }

    // Книжная заявка (701) обязана быть в базе с остатком после частичного
    // исполнения (10 - 4 = 6), а не с исходным объёмом и не отсутствовать
    // вовсе.
    PgResult buyOrder = connOpt->execute(
        "SELECT side, price, initial_quantity, remaining_quantity, status FROM orders "
        "WHERE order_id = $1",
        {std::optional<std::string>("701")});
    ASSERT_EQ(buyOrder.rowCount(), 1);
    EXPECT_EQ(buyOrder.getValue(0, 0), "BUY");
    EXPECT_EQ(buyOrder.getValue(0, 1), "100");
    EXPECT_EQ(buyOrder.getValue(0, 2), "10");
    EXPECT_EQ(buyOrder.getValue(0, 3), "6");
    EXPECT_EQ(buyOrder.getValue(0, 4), "PARTIALLY_FILLED");

    PgResult sellOrder = connOpt->execute(
        "SELECT status, remaining_quantity FROM orders WHERE order_id = $1",
        {std::optional<std::string>("702")});
    ASSERT_EQ(sellOrder.rowCount(), 1);
    EXPECT_EQ(sellOrder.getValue(0, 0), "FILLED");
    EXPECT_EQ(sellOrder.getValue(0, 1), "0");

    PgResult trade = connOpt->execute(
        "SELECT price, quantity FROM trades WHERE buy_order_id = $1 AND sell_order_id = $2",
        {std::optional<std::string>("701"), std::optional<std::string>("702")});
    ASSERT_EQ(trade.rowCount(), 1);
    EXPECT_EQ(trade.getValue(0, 0), "100");
    EXPECT_EQ(trade.getValue(0, 1), "4");

    // Обе команды обязаны осесть в processed_commands (REQ-COMPAT-01 требует
    // сохранение именно "обработанных команд", это отдельная таблица от
    // orders/trades и отдельный путь записи — идемпотентность). Строка
    // должна не просто существовать, но и нести непустой result: там лежит
    // сериализованный ExecutionResult, без которого повтор command_id не
    // смог бы вернуть тот же ответ клиенту.
    // Проверяется не только непустота: строка "{}" непуста, но эффекта
    // команды не несёт, и повтор command_id по такой записи вернул бы
    // клиенту пустой ответ вместо исходного. Поэтому result разбирается и
    // сверяется по существу — сделка у SELL-команды и снимок заявки у BUY.
    PgResult buyCommand = connOpt->execute(
        "SELECT result FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("cmd-persist-buy")});
    ASSERT_EQ(buyCommand.rowCount(), 1);
    ASSERT_FALSE(buyCommand.isNull(0, 0));
    const nlohmann::json buyStored = nlohmann::json::parse(buyCommand.getValue(0, 0));
    // BUY встала в книгу, ни с чем не скрестившись: сделок нет, но снимок
    // самой заявки в эффекте команды быть обязан.
    EXPECT_TRUE(buyStored.at("trades").empty());
    EXPECT_FALSE(buyStored.at("order_changes").empty());

    PgResult sellCommand = connOpt->execute(
        "SELECT result FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("cmd-persist-sell")});
    ASSERT_EQ(sellCommand.rowCount(), 1);
    ASSERT_FALSE(sellCommand.isNull(0, 0));
    const nlohmann::json sellStored = nlohmann::json::parse(sellCommand.getValue(0, 0));
    ASSERT_EQ(sellStored.at("trades").size(), 1u);
    EXPECT_EQ(sellStored.at("trades")[0].at("price"), 100);
    EXPECT_EQ(sellStored.at("trades")[0].at("quantity"), 4);
}

// REQ-COMPAT-04: цену сделки задаёт заявка, уже
// стоявшая в книге (matching_engine.cpp, executeTrade: bookOrder->getPrice()),
// а не входящая. BUY и SELL заведены по разным ценам специально: при ошибке
// "цена входящей заявки" сделка получила бы цену 95 вместо 100 — отличие
// видно и в ответе клиенту, и в записи БД, тест проверяет оба места.
TEST(NetworkTest, TradePriceComesFromRestingBookOrderNotIncomingOrder) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    {
        // Область видимости обязана закончиться (и присоединить io-поток
        // сервера) раньше прямого запроса к *connOpt ниже — то же
        // обязательство, что и в тесте сохранения ADD в базу
        // (AddOverSocketPersistsOrdersAndTradesToDatabase): libpq не
        // допускает работу с одним соединением из двух потоков одновременно.
        TestServer testServer(4096, &*connOpt);
        Client client(4096);
        client.connect("127.0.0.1", testServer.port());

        nlohmann::json buy;
        buy["type"] = "ADD";
        buy["order_id"] = 801;
        buy["side"] = "BUY";
        buy["price"] = 100;
        buy["quantity"] = 5;
        buy["command_id"] = "cmd-price-buy";
        ASSERT_EQ(client.request(buy).at("status"), "OK");

        // Цена входящей SELL (95) заведомо ниже книжной BUY (100) — заявки
        // всё равно скрещиваются (95 <= 100), но верная цена сделки — цена
        // заявки из книги (100), а не входящей (95).
        nlohmann::json sell;
        sell["type"] = "ADD";
        sell["order_id"] = 802;
        sell["side"] = "SELL";
        sell["price"] = 95;
        sell["quantity"] = 5;
        sell["command_id"] = "cmd-price-sell";
        const nlohmann::json response = client.request(sell);

        ASSERT_EQ(response.at("status"), "OK");
        ASSERT_EQ(response.at("trades").size(), 1u);
        EXPECT_EQ(response.at("trades")[0].at("price"), 100);
    }

    PgResult trade = connOpt->execute(
        "SELECT price FROM trades WHERE buy_order_id = $1 AND sell_order_id = $2",
        {std::optional<std::string>("801"), std::optional<std::string>("802")});
    ASSERT_EQ(trade.rowCount(), 1);
    EXPECT_EQ(trade.getValue(0, 0), "100");
}

// REQ-TEST-06: восстановление состояния через перезапуск сервера. Старый
// экземпляр сервера разрушается целиком, новый создаётся заново на том же
// соединении с базой, а книгу запрашивает новый клиент; среди
// восстановленных заявок есть частично исполненная (проверяется остаток, а
// не исходный объём), и проверяется порядок заявок внутри одного ценового
// уровня, а не только их состав.
//
// Тест намеренно прогоняется на "грязной" базе — до того, как первый
// экземпляр сервера вообще стартует, в таблицу orders напрямую (в обход
// обычного командного пути) кладётся посторонняя активная заявка, как будто
// оставленная другим, уже завершившимся процессом. Восстановление читает всю
// таблицу активных заявок целиком, а не только те, что добавил сам тест, —
// эта заявка обязана оказаться в снимке книги после перезапуска наравне с
// остальными.
TEST(NetworkTest, RestartRecoversPartialFillAndPriorityOrderOnDirtyDatabase) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    // Посторонняя заявка: цена (999) вне диапазона остальных цен теста —
    // чтобы не участвовать в сопоставлении и не сместить остальные проверки,
    // но остаться на дальнем конце sell-стороны книги.
    {
        OrderChange strayOrder;
        strayOrder.id = 900001;
        strayOrder.side = Side::Sell;
        strayOrder.price = 999;
        strayOrder.initialQuantity = 2;
        strayOrder.remainingQuantity = 2;
        strayOrder.status = OrderStatus::Open;
        strayOrder.sequenceNumber = 1;
        OrderRepository().save(*connOpt, strayOrder);
    }

    {
        // Первый экземпляр сервера восстанавливает книгу при старте, как и
        // предписывает REQ-NET-14, — тем же recoverState(), которым
        // пользуется server_main.cpp, поэтому посторонняя заявка сразу видна
        // в его книге, а не только после второго перезапуска.
        //
        // ManualServer (а не голые Server/io_context/std::thread) гарантирует
        // Server::stop() и join() даже при досрочном возврате из ASSERT_*
        // ниже — без этой гарантии упавшая на середине проверка уносила бы
        // процесс в std::terminate() из-за неприсоединённого потока, а не
        // давала понятный красный тест.
        CommandProcessor processor1;
        recoverState(*connOpt, processor1);

        RequestRouter router1(processor1, &*connOpt);
        ManualServer serverWrap1(router1, 4096);

        Client client(4096);
        client.connect("127.0.0.1", serverWrap1.port());

        // order_id 2 и order_id 1 делят один ценовой уровень (100). order_id
        // намеренно не совпадает с порядком поступления: первой приходит
        // заявка с БОЛЬШИМ идентификатором (2), второй — с меньшим (1).
        // Реализация, потерявшая приоритет по времени и вместо него
        // сортирующая книгу по возрастанию order_id, вернула бы обратный
        // порядок (1, затем 2) — старая расстановка идентификаторов (1
        // раньше 2) этого бы не поймала, потому что совпадала с сортировкой
        // по возрастанию id случайно.
        nlohmann::json buy1;
        buy1["type"] = "ADD";
        buy1["order_id"] = 2;
        buy1["side"] = "BUY";
        buy1["price"] = 100;
        buy1["quantity"] = 10;
        buy1["command_id"] = "cmd-restart-buy1";
        ASSERT_EQ(client.request(buy1).at("status"), "OK");

        nlohmann::json buy2;
        buy2["type"] = "ADD";
        buy2["order_id"] = 1;
        buy2["side"] = "BUY";
        buy2["price"] = 100;
        buy2["quantity"] = 5;
        buy2["command_id"] = "cmd-restart-buy2";
        ASSERT_EQ(client.request(buy2).at("status"), "OK");

        // Частично исполняет order_id 2 (раннюю по времени заявку на уровне
        // 100): 10 -> 6; order_id 1 не затронут.
        nlohmann::json sell3;
        sell3["type"] = "ADD";
        sell3["order_id"] = 3;
        sell3["side"] = "SELL";
        sell3["price"] = 100;
        sell3["quantity"] = 4;
        sell3["command_id"] = "cmd-restart-sell3";
        const nlohmann::json tradeResponse = client.request(sell3);
        ASSERT_EQ(tradeResponse.at("status"), "OK");
        ASSERT_EQ(tradeResponse.at("trades").size(), 1u);
        EXPECT_EQ(tradeResponse.at("trades")[0].at("buy_order_id"), 2);

        // Остаётся в книге непересекающейся ценой (105 < 999 посторонней
        // заявки, но выше лучшей BUY 100 — не матчится ни с чем).
        nlohmann::json sell4;
        sell4["type"] = "ADD";
        sell4["order_id"] = 4;
        sell4["side"] = "SELL";
        sell4["price"] = 105;
        sell4["quantity"] = 7;
        sell4["command_id"] = "cmd-restart-sell4";
        ASSERT_EQ(client.request(sell4).at("status"), "OK");

        // Клиент дождался ответа на все команды до того, как инициируется
        // остановка блока (деструктор serverWrap1 ниже), — тест намеренно не
        // заходит в узкое окно "кадр принят ровно в момент сигнала"
        // (docs/task4/01-service-lifecycle.md, раздел 4.2): это отдельное,
        // осознанно не устраняемое поведение системы, а не то, что здесь
        // проверяется.
    }
    // Первый экземпляр сервера разрушен целиком (деструктор ManualServer
    // остановил Server и присоединил io-поток) — Server, CommandProcessor и
    // RequestRouter вышли из области видимости. Но не всё общее исчезает
    // вместе с ними: SequenceGenerator::instance() и Logger::instance() —
    // процессные синглтоны, и второй экземпляр пользуется теми же
    // объектами, что и первый, а не независимой копией.
    //
    // Отсюда ограничение этого теста: из-за общего SequenceGenerator он не
    // способен заметить пропажу восстановления счётчика последовательности
    // при старте — к моменту создания processor2 счётчик в этом процессе и
    // так уже стоит в нужном значении, независимо от того, читает ли
    // recoverState() что-либо о последнем sequence_number из БД. При
    // настоящем перезапуске процесса (systemctl restart, REQ-COMPAT-03)
    // этой поблажки нет — там SequenceGenerator стартует заново, и такую
    // регрессию способен поймать только перезапуск процесса целиком, что
    // проверяется отдельно.

    CommandProcessor processor2;
    recoverState(*connOpt, processor2);

    RequestRouter router2(processor2, &*connOpt);
    ManualServer serverWrap2(router2, 4096);

    // Новый клиент — отдельное соединение, никак не связанное с тем, что
    // добавляло заявки до остановки.
    Client newClient(4096);
    newClient.connect("127.0.0.1", serverWrap2.port());
    const nlohmann::json response =
        newClient.request(nlohmann::json::parse(R"({"type":"PRINT"})"));
    ASSERT_EQ(response.at("status"), "OK");

    const auto& buy = response.at("result").at("buy");
    const auto& sell = response.at("result").at("sell");

    // Остаток частично исполненной заявки, а не исходный объём (6, не 10),
    // и порядок внутри ценового уровня: order_id 2 раньше order_id 1 — FIFO
    // по времени поступления, сохранённый через sequence_number и явную
    // сортировку при восстановлении (ORDER BY sequence_number ASC,
    // order_repository.cpp). Цена проверяется по обеим сторонам книги —
    // раздел 3.4 контракта фиксирует в записи снимка три поля (order_id,
    // price, quantity), а не два.
    ASSERT_EQ(buy.size(), 2u);
    EXPECT_EQ(buy[0].at("order_id"), 2);
    EXPECT_EQ(buy[0].at("price"), 100);
    EXPECT_EQ(buy[0].at("quantity"), 6);
    EXPECT_EQ(buy[1].at("order_id"), 1);
    EXPECT_EQ(buy[1].at("price"), 100);
    EXPECT_EQ(buy[1].at("quantity"), 5);

    // sell: order_id 4 (цена 105) раньше посторонней заявки (цена 999) —
    // возрастание цены. Посторонняя заявка тоже восстановлена, а не только
    // заявки, добавленные этим тестом (тест прогоняется на грязной базе).
    ASSERT_EQ(sell.size(), 2u);
    EXPECT_EQ(sell[0].at("order_id"), 4);
    EXPECT_EQ(sell[0].at("price"), 105);
    EXPECT_EQ(sell[0].at("quantity"), 7);
    EXPECT_EQ(sell[1].at("order_id"), 900001);
    EXPECT_EQ(sell[1].at("price"), 999);
    EXPECT_EQ(sell[1].at("quantity"), 2);
}

// REQ-COMPAT-04, третье доменное правило: рыночная заявка не попадает в
// книгу. ModifyAndMarketOrderWorkOverNetwork (выше) исполняет MARKET SELL
// целиком и книгу после этого не проверяет, поэтому реализация, кладущая
// неисполненный остаток MARKET-заявки в книгу как обычный лимитный ордер,
// прошла бы тот тест незамеченной. Здесь ликвидности в книге заведомо
// меньше, чем в MARKET-заявке (BUY на 5, MARKET SELL на 8): сделка
// исполняется на доступные 5, а непокрытый остаток (3) обязан быть просто
// потерян — PRINT после этого обязан показать пустую книгу с обеих сторон,
// а не BUY-остаток нулевой (уже исполнен целиком выше по цепочке) и не
// SELL-остаток из недостающих 3 единиц MARKET-заявки.
TEST(NetworkTest, MarketOrderWithInsufficientLiquidityIsNotAddedToBook) {
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
    buy["order_id"] = 20;
    buy["side"] = "BUY";
    buy["price"] = 100;
    buy["quantity"] = 5;
    buy["command_id"] = "cmd-liquidity-buy";
    ASSERT_EQ(client.request(buy).at("status"), "OK");

    // Снимок до рыночной заявки. Без него пустая книга в конце теста была бы
    // единственным наблюдением, и реализация, у которой PRINT всегда
    // возвращает пусто, прошла бы проверку насквозь.
    const nlohmann::json printRequest = nlohmann::json::parse(R"({"type":"PRINT"})");
    const nlohmann::json bookBefore = client.request(printRequest);
    ASSERT_EQ(bookBefore.at("status"), "OK");
    ASSERT_EQ(bookBefore.at("result").at("buy").size(), 1u);
    EXPECT_EQ(bookBefore.at("result").at("buy")[0].at("order_id"), 20);
    EXPECT_EQ(bookBefore.at("result").at("buy")[0].at("quantity"), 5);

    nlohmann::json marketSell;
    marketSell["type"] = "ADD";
    marketSell["order_type"] = "MARKET";
    marketSell["order_id"] = 21;
    marketSell["side"] = "SELL";
    marketSell["quantity"] = 8;
    marketSell["command_id"] = "cmd-liquidity-market-sell";
    const nlohmann::json marketResponse = client.request(marketSell);

    EXPECT_EQ(marketResponse.at("status"), "OK");
    ASSERT_EQ(marketResponse.at("trades").size(), 1u);
    EXPECT_EQ(marketResponse.at("trades")[0].at("buy_order_id"), 20);
    EXPECT_EQ(marketResponse.at("trades")[0].at("sell_order_id"), 21);
    EXPECT_EQ(marketResponse.at("trades")[0].at("price"), 100);
    EXPECT_EQ(marketResponse.at("trades")[0].at("quantity"), 5);

    const nlohmann::json printResponse = client.request(printRequest);
    ASSERT_EQ(printResponse.at("status"), "OK");
    EXPECT_TRUE(printResponse.at("result").at("buy").empty());
    EXPECT_TRUE(printResponse.at("result").at("sell").empty());
}

// Несколько одновременных клиентов, таймаут чтения, HEALTH
// (REQ-EXT-01, REQ-EXT-02, REQ-EXT-04..REQ-EXT-07).

// Три клиента подключены одновременно (ни один сокет не
// закрыт до открытия следующего), каждый выполняет команды над общей
// книгой, и итоговое состояние книги — сумма их действий: сделка между
// заявками с РАЗНЫХ соединений, а PRINT с третьего соединения видит эффект
// первых двух.
TEST(NetworkTest, ThreeSimultaneousClientsShareOneConsistentOrderBook) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    TestServer testServer(4096, &*connOpt);

    Client client1(4096);
    Client client2(4096);
    Client client3(4096);
    // Все три соединения открыты в один и тот же момент — ни одно не
    // закрывается перед подключением следующего.
    client1.connect("127.0.0.1", testServer.port());
    client2.connect("127.0.0.1", testServer.port());
    client3.connect("127.0.0.1", testServer.port());

    nlohmann::json buy;
    buy["type"] = "ADD";
    buy["order_id"] = 1;
    buy["side"] = "BUY";
    buy["price"] = 100;
    buy["quantity"] = 10;
    buy["command_id"] = "cmd-concurrent-1";
    ASSERT_EQ(client1.request(buy).at("status"), "OK");

    // Сделка между заявками с разных соединений: SELL пришёл по client2,
    // BUY выше — по client1.
    nlohmann::json sell;
    sell["type"] = "ADD";
    sell["order_id"] = 2;
    sell["side"] = "SELL";
    sell["price"] = 100;
    sell["quantity"] = 4;
    sell["command_id"] = "cmd-concurrent-2";
    const nlohmann::json sellResponse = client2.request(sell);
    ASSERT_EQ(sellResponse.at("status"), "OK");
    ASSERT_EQ(sellResponse.at("trades").size(), 1u);
    EXPECT_EQ(sellResponse.at("trades")[0].at("buy_order_id"), 1);
    EXPECT_EQ(sellResponse.at("trades")[0].at("sell_order_id"), 2);

    // Третье соединение добавляет ещё одну заявку на непересекающейся цене.
    nlohmann::json buy3;
    buy3["type"] = "ADD";
    buy3["order_id"] = 3;
    buy3["side"] = "BUY";
    buy3["price"] = 99;
    buy3["quantity"] = 1;
    buy3["command_id"] = "cmd-concurrent-3";
    ASSERT_EQ(client3.request(buy3).at("status"), "OK");

    // Книга — сумма действий всех трёх: остаток BUY 1 (10 - 4 = 6) на цене
    // 100 и целиком BUY 3 на цене 99; SELL пуста (заявка 2 исполнилась
    // целиком). Любое из трёх соединений видит одну и ту же общую книгу.
    const nlohmann::json book = client3.request(nlohmann::json::parse(R"({"type":"PRINT"})"));
    ASSERT_EQ(book.at("status"), "OK");
    const auto& buySide = book.at("result").at("buy");
    ASSERT_EQ(buySide.size(), 2u);
    EXPECT_EQ(buySide[0].at("order_id"), 1);
    EXPECT_EQ(buySide[0].at("quantity"), 6);
    EXPECT_EQ(buySide[1].at("order_id"), 3);
    EXPECT_EQ(buySide[1].at("quantity"), 1);
    EXPECT_TRUE(book.at("result").at("sell").empty());
}

// Server не ограничивает число одновременных соединений
// искусственно — много клиентов подключены одновременно и все обслужены, ни
// один не получил отказа и не ждал освобождения места. БД не нужна: команда
// — PING, число соединений — единственное, что проверяется.
TEST(NetworkTest, ServerAcceptsManySimultaneousConnectionsWithoutArtificialLimit) {
    TestServer testServer(1024);

    constexpr int kClientCount = 25;
    std::vector<std::unique_ptr<Client>> clients;
    clients.reserve(kClientCount);
    // Подключаются все разом, прежде чем хоть один из них что-то отправит —
    // ни одно соединение не ждёт закрытия предыдущего, чтобы освободить
    // место.
    for (int i = 0; i < kClientCount; ++i) {
        auto client = std::make_unique<Client>(1024);
        client->connect("127.0.0.1", testServer.port());
        clients.push_back(std::move(client));
    }

    for (int i = 0; i < kClientCount; ++i) {
        const nlohmann::json response =
            clients[i]->request(nlohmann::json::parse(R"({"type":"PING"})"));
        EXPECT_EQ(response.at("status"), "OK") << "клиент " << i;
        EXPECT_EQ(response.at("result"), "PONG") << "клиент " << i;
    }
}

// Вторая половина REQ-EXT-01: у каждого соединения собственная Session, а не
// разделяемое состояние — ошибка, закрывающая одно соединение (превышение
// max_message_size), не задевает второе, всё это время остававшееся
// открытым и нетронутым.
TEST(NetworkTest, ClosingOneSessionOnProtocolErrorDoesNotAffectAnotherOpenSession) {
    constexpr std::size_t kServerLimit = 256;
    TestServer testServer(kServerLimit);

    Client client1(4096);
    Client client2(1024);
    client1.connect("127.0.0.1", testServer.port());
    client2.connect("127.0.0.1", testServer.port());

    // Оба соединения приняты и работают до того, как одно из них сломается.
    ASSERT_EQ(client1.request(nlohmann::json::parse(R"({"type":"PING"})")).at("status"), "OK");
    ASSERT_EQ(client2.request(nlohmann::json::parse(R"({"type":"PING"})")).at("status"), "OK");

    nlohmann::json bigRequest;
    bigRequest["type"] = "PING";
    bigRequest["padding"] = std::string(300, 'x');
    client1.sendRawBytes(client1.encodeFrame(bigRequest.dump()));
    const nlohmann::json errorResponse = nlohmann::json::parse(client1.receiveFrame());
    EXPECT_EQ(errorResponse.at("error"), "MESSAGE_TOO_LARGE");
    EXPECT_THROW(client1.receiveFrame(), NetworkError)
        << "сессия client1 обязана была закрыться после MESSAGE_TOO_LARGE";

    // client2 всё это время был открыт и его не тронули — его собственная
    // Session не заметила ничего из происходившего с client1.
    const nlohmann::json pong2 = client2.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(pong2.at("status"), "OK");
    EXPECT_EQ(pong2.at("result"), "PONG");
}

// HEALTH (REQ-EXT-06, REQ-EXT-07).

// Формат ответа совпадает с контрактом побайтово — ровно три
// поля с этими значениями, ничего лишнего (в частности, command_id из
// запроса не эхируется, как и у PING).
TEST(NetworkTest, HealthReturnsExactContractShapeWithLiveDatabase) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    TestServer testServer(1024, &*connOpt);
    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    const nlohmann::json response = client.request(nlohmann::json::parse(
        R"({"type":"HEALTH","command_id":"cmd-health-ignored"})"));

    const nlohmann::json expected = nlohmann::json::parse(
        R"({"status":"OK","database":"CONNECTED","engine":"READY"})");
    EXPECT_EQ(response, expected);
}

// HEALTH не требует command_id, не пишет в processed_commands
// и не трогает книгу — снимок PRINT до и после совпадает, а единственная
// строка в processed_commands принадлежит ADD, отправленному до HEALTH.
TEST(NetworkTest, HealthDoesNotChangeEngineOrDatabaseState) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    cleanupAllTables(*connOpt);

    {
        // Область видимости обязана закончиться (и присоединить io-поток
        // сервера) раньше прямого запроса к *connOpt ниже — то же
        // обязательство, что и в остальных тестах файла: libpq не
        // допускает работу с одним соединением из двух потоков.
        TestServer testServer(4096, &*connOpt);
        Client client(4096);
        client.connect("127.0.0.1", testServer.port());

        nlohmann::json add;
        add["type"] = "ADD";
        add["order_id"] = 1;
        add["side"] = "BUY";
        add["price"] = 100;
        add["quantity"] = 5;
        add["command_id"] = "cmd-health-state-add";
        ASSERT_EQ(client.request(add).at("status"), "OK");

        const nlohmann::json printRequest = nlohmann::json::parse(R"({"type":"PRINT"})");
        const nlohmann::json bookBefore = client.request(printRequest);

        // Запрос без command_id — контракт его не требует.
        const nlohmann::json health =
            client.request(nlohmann::json::parse(R"({"type":"HEALTH"})"));
        EXPECT_EQ(health.at("status"), "OK");

        const nlohmann::json bookAfter = client.request(printRequest);
        EXPECT_EQ(bookBefore, bookAfter);
    }

    PgResult commandCount = connOpt->execute("SELECT COUNT(*) FROM processed_commands");
    ASSERT_EQ(commandCount.rowCount(), 1);
    // Единственная команда, реально прошедшая через CommandProcessor, — ADD
    // выше; ни HEALTH, ни PRINT строк в processed_commands не оставляют.
    EXPECT_EQ(commandCount.getValue(0, 0), "1");
}

// Поле database вычисляется, а не является литералом —
// значение отличается от "CONNECTED", когда соединение с базой не
// сконфигурировано (RequestRouter построен с connection == nullptr, самая
// дешёвая точка различения, которой уже пользуются тесты формата кадра
// файла).
TEST(NetworkTest, HealthReportsNonConnectedDatabaseWhenNoConnectionIsConfigured) {
    TestServer testServer(1024);
    Client client(1024);
    client.connect("127.0.0.1", testServer.port());

    const nlohmann::json response =
        client.request(nlohmann::json::parse(R"({"type":"HEALTH"})"));
    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("engine"), "READY");
    EXPECT_NE(response.at("database"), "CONNECTED");
}

// Таймаут чтения (REQ-EXT-04, REQ-EXT-05).

// Клиент подключился и не присылает данных — по истечении
// таймаута сервер закрывает соединение, и клиент наблюдает конец потока
// (событие, а не время: receiveFrame() блокируется на чтении, а не на
// ожидании часов).
TEST(NetworkTest, IdleConnectionIsClosedAfterReadTimeout) {
    constexpr std::chrono::seconds kReadTimeout(1);
    TestServer testServer(1024, nullptr, kReadTimeout);

    // Собственный таймаут клиента заведомо больше серверного — иначе тест
    // не отличил бы "сервер закрыл соединение" от "клиент сам сдался,
    // не дождавшись ответа".
    Client client(1024, std::chrono::seconds(5));
    client.connect("127.0.0.1", testServer.port());

    try {
        client.receiveFrame();
        FAIL() << "сервер обязан был закрыть простаивающее соединение по таймауту";
    } catch (const NetworkTimeoutError&) {
        FAIL() << "соединение не было закрыто сервером — таймаут чтения не сработал";
    } catch (const NetworkError&) {
        SUCCEED();
    }
}

// Замечание ревью, находка 2: истечение readTimeout_ обязано быть жёстким
// дедлайном (REQ-EXT-04), а не ожиданием, пока опустеет очередь записи.
// Клиент запрашивает PRINT над книгой, чей ответ весит несколько мегабайт и
// не помещается в буферы TCP на localhost (тот же приём, что и в
// StopForcesShutdownAfterDeadlineWhenWriteNeverDrains ниже), читает только
// заголовок ответа и дальше выжидает дольше readTimeout, не читая и не
// записывая ничего, — очередь записи сессии всё это время остаётся
// недренированной. Только после паузы клиент впервые пытается дочитать
// оставшееся тело: если бы истечение readTimeout лишь взводило
// closeAfterWrite_ (как раньше) вместо безусловного закрытия сокета, сессия
// всё это время молча ждала бы и, как только клиент наконец начал бы читать,
// охотно доотправила бы ответ целиком — то есть тест обнаружил бы не обрыв,
// а полностью доставленный ответ. С исправлением сокет обязан быть закрыт
// сервером уже к началу чтения (не позже readTimeout после начала паузы),
// поэтому клиент получает заведомо МЕНЬШЕ полного тела и обрыв вместо
// оставшихся байт.
TEST(NetworkTest, ReadTimeoutClosesSessionEvenWhileResponseNeverDrains) {
    constexpr std::size_t kServerLimit = 32u * 1024u * 1024u;
    constexpr std::chrono::seconds kReadTimeout(1);
    CommandProcessor processor;

    constexpr int kOrderCount = 100000;
    for (int i = 0; i < kOrderCount; ++i) {
        processor.restoreOrder(std::make_shared<Order>(
            i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
    }

    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, kServerLimit};
    boost::asio::io_context ioContext;
    Server server(ioContext, config, router, std::chrono::seconds(10), kReadTimeout);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    boost::asio::io_context clientIoContext;
    boost::asio::ip::tcp::socket clientSocket(clientIoContext);
    clientSocket.connect(boost::asio::ip::tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), server.port()));

    const MessageCodec codec(kServerLimit);
    boost::asio::write(clientSocket, boost::asio::buffer(codec.encode(R"({"type":"PRINT"})")));

    std::array<char, kFrameHeaderSize> header{};
    readExactWithDeadline(clientSocket, boost::asio::buffer(header), std::chrono::seconds(5));
    const std::uint32_t bodySize = codec.decodeHeader(header);

    // Пауза, заведомо большая kReadTimeout: клиент во время неё ничего не
    // читает и не пишет — то самое "клиент, переставший и читать, и писать",
    // от которого обязан защищать REQ-EXT-04. Реальное течение времени
    // здесь неизбежно (см. комментарий у waitBriefly выше) — тест по своей
    // сути проверяет исход гонки с настоящим таймером.
    constexpr std::chrono::milliseconds kPause(2000);
    waitBriefly(kPause);

    // Только теперь клиент впервые пытается дочитать оставшееся. Если
    // соединение всё ещё было бы открыто (регрессия), это чтение
    // разблокировало бы застрявшую запись и получило бы ответ целиком —
    // ровно поэтому проверяется не сам факт обрыва, а то, что дошло заведомо
    // меньше полного тела.
    std::promise<std::size_t> receivedPromise;
    std::future<std::size_t> receivedFuture = receivedPromise.get_future();
    std::thread reader([&clientSocket, &receivedPromise] {
        std::vector<char> buffer(1u << 16);
        std::size_t total = 0;
        boost::system::error_code ec;
        while (!ec) {
            const std::size_t transferred =
                clientSocket.read_some(boost::asio::buffer(buffer), ec);
            total += transferred;
        }
        receivedPromise.set_value(total);
    });

    constexpr std::chrono::seconds kWait(10);
    if (receivedFuture.wait_for(kWait) != std::future_status::ready) {
        ADD_FAILURE() << "чтение оставшегося тела не завершилось обрывом за"
                          " отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    reader.join();

    const std::size_t received = receivedFuture.get();
    EXPECT_LT(received, static_cast<std::size_t>(bodySize))
        << "ответ дошёл целиком — соединение не было закрыто по readTimeout,"
           " пока очередь записи не опустела сама";

    boost::system::error_code ignored;
    clientSocket.close(ignored);

    server.stop();
    std::promise<void> stopped;
    std::future<void> stoppedFuture = stopped.get_future();
    std::thread joiner([&ioThread, &stopped] {
        ioThread.join();
        stopped.set_value();
    });
    if (stoppedFuture.wait_for(kWait) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    joiner.join();
}

// Тот же дедлайн обязан действовать и после того, как сессия решила больше
// не читать по своей причине — не из-за остановки сервера. Такое состояние
// (closeAfterWrite_ вне остановки) наступает на нарушении протокола
// (MESSAGE_TOO_LARGE) и на сбое сохранения в БД: сессия ждёт, пока очередь
// записи допишет накопленный ответ, и закрывается сама.
//
// Найдено аудитом требований: до правки таймер на этих путях СНИМАЛСЯ, и
// оставался путь, на котором соединение висит вечно — клиент, переставший и
// читать, и писать, не даёт async_write завершиться, а снятый таймер больше
// ничего не ограничивает. То есть REQ-EXT-04 выполнялся для сессии,
// ожидающей кадр, но не для сессии, ожидающей отправки. При остановке
// сервера этот случай прикрыт предельным временем остановки (REQ-EXT-09), в
// обычной работе — не был прикрыт ничем.
//
// Устройство теста повторяет предыдущий: клиент забирает только заголовок
// огромного ответа на PRINT, оставляя очередь записи заведомо непустой, и
// лишь затем присылает кадр с завышенным заголовком — сессия переходит в
// closeAfterWrite_ и остаётся ждать отправки.
TEST(NetworkTest, ReadTimeoutClosesSessionWaitingToDrainAfterProtocolError) {
    constexpr std::size_t kServerLimit = 32u * 1024u * 1024u;
    constexpr std::chrono::seconds kReadTimeout(1);
    CommandProcessor processor;

    constexpr int kOrderCount = 100000;
    for (int i = 0; i < kOrderCount; ++i) {
        processor.restoreOrder(std::make_shared<Order>(
            i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
    }

    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, kServerLimit};
    boost::asio::io_context ioContext;
    Server server(ioContext, config, router, std::chrono::seconds(10), kReadTimeout);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    boost::asio::io_context clientIoContext;
    boost::asio::ip::tcp::socket clientSocket(clientIoContext);
    clientSocket.connect(boost::asio::ip::tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), server.port()));

    const MessageCodec codec(kServerLimit);
    boost::asio::write(clientSocket, boost::asio::buffer(codec.encode(R"({"type":"PRINT"})")));

    std::array<char, kFrameHeaderSize> header{};
    readExactWithDeadline(clientSocket, boost::asio::buffer(header), std::chrono::seconds(5));
    const std::uint32_t bodySize = codec.decodeHeader(header);

    // Заголовок объявляет размер больше серверного предела — нарушение
    // протокола (REQ-PROTO-10). Кадр собирается вручную: MessageCodec такой
    // кадр закодировать откажется, он сам проверяет предел.
    std::array<unsigned char, kFrameHeaderSize> oversized{};
    const std::uint32_t declared = static_cast<std::uint32_t>(kServerLimit) + 1u;
    oversized[0] = static_cast<unsigned char>((declared >> 24) & 0xFFu);
    oversized[1] = static_cast<unsigned char>((declared >> 16) & 0xFFu);
    oversized[2] = static_cast<unsigned char>((declared >> 8) & 0xFFu);
    oversized[3] = static_cast<unsigned char>(declared & 0xFFu);
    boost::asio::write(clientSocket, boost::asio::buffer(oversized));

    // Дальше клиент неподвижен дольше kReadTimeout — тот же случай и та же
    // причина неизбежности реального времени, что у предыдущего теста.
    constexpr std::chrono::milliseconds kPause(2000);
    waitBriefly(kPause);

    std::promise<std::size_t> receivedPromise;
    std::future<std::size_t> receivedFuture = receivedPromise.get_future();
    std::thread reader([&clientSocket, &receivedPromise] {
        std::vector<char> buffer(1u << 16);
        std::size_t total = 0;
        boost::system::error_code ec;
        while (!ec) {
            const std::size_t transferred =
                clientSocket.read_some(boost::asio::buffer(buffer), ec);
            total += transferred;
        }
        receivedPromise.set_value(total);
    });

    constexpr std::chrono::seconds kWaitDrain(10);
    if (receivedFuture.wait_for(kWaitDrain) != std::future_status::ready) {
        ADD_FAILURE() << "сессия, ожидавшая отправки после нарушения протокола,"
                         " не была закрыта по readTimeout";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    reader.join();

    const std::size_t drained = receivedFuture.get();
    EXPECT_LT(drained, static_cast<std::size_t>(bodySize))
        << "ответ дошёл целиком — значит сессию закрыла опустевшая очередь"
           " записи, а не дедлайн, и путь без дедлайна остался бы незамеченным";

    boost::system::error_code closeIgnored;
    clientSocket.close(closeIgnored);

    server.stop();
    std::promise<void> drainStopped;
    std::future<void> drainStoppedFuture = drainStopped.get_future();
    std::thread drainJoiner([&ioThread, &drainStopped] {
        ioThread.join();
        drainStopped.set_value();
    });
    if (drainStoppedFuture.wait_for(kWaitDrain) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    drainJoiner.join();
}

// Клиент, присылающий команды чаще таймаута, работает сколь
// угодно долго — таймер сбрасывается при каждом успешном чтении, а не
// отсчитывается от подключения. Замечание ревью: пауза между раундами,
// организованная ожиданием на никогда не наполняемом future
// (std::promise::get_future().wait_for()), семантически неотличима от
// std::this_thread::sleep_for и делает запрет sleep_for
// проверкой орфографии, а не поведения. Вместо паузы команды идут плотным
// циклом без остановок, а доказательством служит суммарное время жизни
// соединения: цикл продолжается, пока оно не превысит kReadTimeout, и после
// этого порога соединение обязано ответить ещё раз. Если бы дедлайн
// отсчитывался один раз от подключения, а не сбрасывался при каждом успешном
// чтении, соединение было бы закрыто до того, как цикл вообще смог бы
// зафиксировать превышение kReadTimeout. Число раундов заранее не
// фиксируется и подстраивается под скорость машины, на которой идёт тест.
TEST(NetworkTest, FrequentCommandsResetReadTimeoutAndSessionStaysOpen) {
    constexpr std::chrono::seconds kReadTimeout(1);
    TestServer testServer(1024, nullptr, kReadTimeout);

    Client client(1024, std::chrono::seconds(5));
    const auto connectTime = std::chrono::steady_clock::now();
    client.connect("127.0.0.1", testServer.port());

    // Предохранитель от бесконечного цикла, если соединение почему-то
    // перестанет отвечать раньше, чем истечёт kReadTimeout, — client.request()
    // сам ограничен таймаутом (5 с) на каждый отдельный раунд, поэтому
    // тест упадёт, а не зависнет, но явная граница числа раундов документирует
    // это намерение.
    constexpr int kMaxRounds = 200000;
    int rounds = 0;
    // Цикл идёт кратно дольше kReadTimeout, а не "чуть дольше". Разница
    // принципиальная и проверена инъекцией: при выходе ровно на границе
    // (elapsed <= kReadTimeout) реализация, взводящая таймер один раз от
    // подключения и не сбрасывающая его при чтении, оставляла тест ЗЕЛЁНЫМ —
    // сессия закрывалась в тот же момент, когда цикл заканчивался, и
    // финальная команда успевала проскочить в те несколько миллисекунд, пока
    // сервер ещё не обработал закрытие. Тест выигрывал гонку, а не доказывал
    // сброс. С тройным запасом такой реализации нечем дотянуть до конца
    // цикла: команды начинают падать внутри него.
    constexpr auto kRunFor = kReadTimeout * 3;
    while (std::chrono::steady_clock::now() - connectTime <= kRunFor) {
        const nlohmann::json response =
            client.request(nlohmann::json::parse(R"({"type":"PING"})"));
        ASSERT_EQ(response.at("status"), "OK") << "раунд " << rounds;
        ASSERT_EQ(response.at("result"), "PONG") << "раунд " << rounds;
        ++rounds;
        ASSERT_LT(rounds, kMaxRounds)
            << "не удалось превысить kReadTimeout за разумное число раундов";
    }

    // Порог кратно пройден — соединение обязано остаться живым и ответить
    // ещё раз: единственное объяснение того, что сессия пережила
    // kReadTimeout, непрерывно отвечая на команды, — таймер сбрасывался при
    // каждом успешном чтении.
    const nlohmann::json finalResponse =
        client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    EXPECT_EQ(finalResponse.at("status"), "OK");
    EXPECT_EQ(finalResponse.at("result"), "PONG");
    EXPECT_GT(std::chrono::steady_clock::now() - connectTime, kReadTimeout)
        << "цикл завершился быстрее readTimeout — тест не доказал превышение";
}

// Остановка сервера с соединением, ожидающим по таймауту,
// завершается штатно — io_context.run() обязана вернуться сама, а не по
// исчерпании shutdownTimeout. readTimeout выбран заведомо больше и
// shutdownTimeout, и отведённого тесту времени ожидания: если beginClose()
// не отменяет таймер сессии, io_context не станет пустой раньше, чем
// сработает один из них, и тест обязан упасть, а не зависнуть.
TEST(NetworkTest, StopReturnsPromptlyEvenWhileSessionWaitsOnReadTimeout) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, 4096};
    boost::asio::io_context ioContext;
    constexpr std::chrono::seconds kShutdownTimeout(10);
    constexpr std::chrono::seconds kReadTimeout(30);
    Server server(ioContext, config, router, kShutdownTimeout, kReadTimeout);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    Client client(4096);
    client.connect("127.0.0.1", server.port());
    // Замечание ревью: client.connect() гарантирует только завершившееся
    // TCP-рукопожатие, а не то, что сервер успел создать Session и взвести
    // её таймер, — обработчик async_accept и запощенная лямбда stop()
    // выполняются в одном и том же io_context, и порядок между ними не
    // определён. Полный обмен PING/PONG устраняет эту гонку: он не может
    // завершиться раньше, чем Session::start() запустит первое чтение
    // (readHeader() -> armReadTimeout()), а ответ на PING приходит только
    // после того, как сессия успела прочитать и обработать запрос —
    // readBody() к этому моменту уже вызвал readHeader() для следующего
    // кадра и заново взвёл таймер.
    const nlohmann::json warmup = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    ASSERT_EQ(warmup.at("status"), "OK");
    // Сессия сейчас снова ждёт следующий кадр — её таймер чтения взведён на
    // kReadTimeout (30 секунд), заведомо больше отведённых тесту 5 секунд.
    server.stop();

    std::promise<void> stopped;
    std::future<void> stoppedFuture = stopped.get_future();
    std::thread joiner([&ioThread, &stopped] {
        ioThread.join();
        stopped.set_value();
    });

    constexpr std::chrono::seconds kTimeout(5);
    if (stoppedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась за отведённое время — "
                          "таймер чтения сессии, вероятно, не был отменён при остановке";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    joiner.join();

    // "Штатно", а не по предельному времени остановки: shutdownTimeout (10
    // секунд) тоже больше отведённых тесту 5 секунд, поэтому само по себе
    // возвращение run() в срок уже исключает его срабатывание, а эта
    // проверка документирует это явно.
    EXPECT_FALSE(server.wasForceStopped());
}

// Замечание ревью: тест выше строит сессию с ПУСТОЙ очередью записи в
// момент stop() (клиент ничего не отправлял, кроме уже завершённого PING) —
// beginClose() идёт веткой closeSocket(), а тот сам первой строкой отменяет
// таймер. Отмена внутри самой beginClose() (её первая строка, до проверки
// writeQueue_.empty()) этим ничем не покрыта: инъекция, убирающая именно её,
// там не заметна. Здесь очередь заведомо НЕ пуста в момент stop() (клиент
// прочитал только заголовок ответа на PRINT, а книга из kOrderCount заявок
// весит несколько мегабайт и не помещается в буферы TCP на localhost, — тот
// же приём, что и в StopForcesShutdownAfterDeadlineWhenWriteNeverDrains
// выше) — на этом пути beginClose() лишь взводит closeAfterWrite_ и
// возвращается, не вызывая closeSocket(): единственное, что может отменить
// таймер, взведённый ДО stop() тем readHeader(), что уже стоял в очереди на
// момент решения об остановке, — explicit-вызов в самой beginClose().
// Клиент намеренно не читает дальше заголовка в течение kPause, заведомо
// большего readTimeout (1 секунда): если бы отмена в beginClose()
// отсутствовала, устаревший таймер сработал бы посреди паузы и
// (после исправления находки 2) безусловно закрыл бы сокет, оборвав ещё не
// законченную отправку ответа — стоп-сигнал не должен
// прерывать уже начатую отправку раньше срока. Предохранитель внутри
// closeSocket() (отдельная отмена там же) не убирается этим тестом и
// продолжает защищать остальные пути закрытия — здесь целенаправленно
// проверяется именно путь "очередь не пуста", которым он не пользуется.
TEST(NetworkTest, StopCancelsStaleReadTimeoutWhileDrainingQueuedResponse) {
    constexpr std::size_t kServerLimit = 32u * 1024u * 1024u;
    CommandProcessor processor;

    constexpr int kOrderCount = 100000;
    for (int i = 0; i < kOrderCount; ++i) {
        processor.restoreOrder(std::make_shared<Order>(
            i + 1, Side::Buy, 100, 1, 1, i + 1, OrderStatus::Open));
    }

    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, kServerLimit};
    boost::asio::io_context ioContext;
    constexpr std::chrono::seconds kShutdownTimeout(10);
    constexpr std::chrono::seconds kReadTimeout(1);
    Server server(ioContext, config, router, kShutdownTimeout, kReadTimeout);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    boost::asio::io_context clientIoContext;
    boost::asio::ip::tcp::socket clientSocket(clientIoContext);
    clientSocket.connect(boost::asio::ip::tcp::endpoint(
        boost::asio::ip::make_address("127.0.0.1"), server.port()));

    const MessageCodec codec(kServerLimit);
    boost::asio::write(clientSocket, boost::asio::buffer(codec.encode(R"({"type":"PRINT"})")));

    std::array<char, kFrameHeaderSize> header{};
    readExactWithDeadline(clientSocket, boost::asio::buffer(header), std::chrono::seconds(5));
    const std::uint32_t bodySize = codec.decodeHeader(header);

    // Очередь записи этой сессии заведомо не пуста: доставлены только
    // четыре байта заголовка из нескольких мегабайт тела.
    server.stop();

    // Пауза, заведомо большая kReadTimeout, — окно, в течение которого
    // устаревший, не отменённый вовремя таймер обязан был бы сработать.
    constexpr std::chrono::milliseconds kPause(2000);
    waitBriefly(kPause);

    // Только теперь клиент дочитывает тело. Обычным readExactWithDeadline
    // здесь не воспользоваться: если устаревший таймер всё же сработал во
    // время паузы и оборвал соединение, ожидаемый исход этого чтения —
    // ЗАКОНОМЕРНАЯ нехватка байт, а не нарушение теста, а readExactWithDeadline
    // в этом случае бросает исключение прямо в тело теста, что разворачивает
    // стек мимо join() ещё не присоединённых ioThread/joiner и валит процесс
    // в std::terminate() вместо чистого красного результата. Поэтому чтение
    // собирается вручную: копится столько байт, сколько сервер успел
    // прислать до обрыва (или ошибки не будет вовсе, если таймер был отменён
    // как положено), после чего расхождение с bodySize проверяется обычным
    // ASSERT_EQ.
    std::promise<std::string> receivedPromise;
    std::future<std::string> receivedFuture = receivedPromise.get_future();
    std::thread bodyReader([&clientSocket, bodySize, &receivedPromise] {
        std::string received;
        received.reserve(bodySize);
        std::vector<char> chunk(1u << 16);
        boost::system::error_code ec;
        while (!ec && received.size() < bodySize) {
            const std::size_t transferred =
                clientSocket.read_some(boost::asio::buffer(chunk), ec);
            received.append(chunk.data(), transferred);
        }
        receivedPromise.set_value(std::move(received));
    });

    constexpr std::chrono::seconds kBodyReadTimeout(10);
    if (receivedFuture.wait_for(kBodyReadTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "чтение оставшегося тела не завершилось за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    bodyReader.join();

    // EXPECT, а не ASSERT: раннего return здесь допустить нельзя — ниже
    // ioThread присоединяется через joiner, и досрочный выход из функции
    // оставил бы ещё не присоединённый std::thread локальной переменной,
    // чей деструктор сам вызывает std::terminate() (та же причина, по
    // которой другие тесты файла с "голым" Server/io_context/std::thread,
    // не завёрнутым в ManualServer, держат ASSERT_* только до объявления
    // такого потока). Поэтому JSON разбирается только при точном совпадении
    // размера — на усечённом теле сам разбор ничего не доказал бы дополнительно.
    const std::string body = receivedFuture.get();
    EXPECT_EQ(body.size(), static_cast<std::size_t>(bodySize))
        << "получено " << body.size() << " из " << bodySize << " байт тела —"
           " устаревший таймер, вероятно, оборвал ещё не законченную отправку";
    if (body.size() == bodySize) {
        const nlohmann::json response = nlohmann::json::parse(body);
        EXPECT_EQ(response.at("status"), "OK");
        EXPECT_EQ(response.at("result").at("buy").size(), static_cast<std::size_t>(kOrderCount));
    }

    // Ответ доставлен целиком — сокет закрыт сервером сразу после него
    // (drain по разделу 4.1 документа), а не молчит и не обрывается раньше
    // срока.
    char extra = 0;
    boost::system::error_code ec;
    const std::size_t transferred =
        boost::asio::read(clientSocket, boost::asio::buffer(&extra, 1), ec);
    EXPECT_EQ(transferred, 0u);
    EXPECT_TRUE(ec);

    std::promise<void> stopped;
    std::future<void> stoppedFuture = stopped.get_future();
    std::thread joiner([&ioThread, &stopped] {
        ioThread.join();
        stopped.set_value();
    });
    constexpr std::chrono::seconds kJoinTimeout(10);
    if (stoppedFuture.wait_for(kJoinTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    joiner.join();

    // Штатное завершение, а не по предельному времени остановки —
    // shutdownTimeout (10 секунд) намного больше суммарного времени этого
    // теста (пауза плюс время на передачу нескольких мегабайт).
    EXPECT_FALSE(server.wasForceStopped());

    boost::system::error_code ignored;
    clientSocket.close(ignored);
}

// Единственная межпоточная операция всего проекта — SignalHandler,
// разбуженный настоящим сигналом, зовущий Server::stop() через post() — до
// сих пор была измерена под ThreadSanitizer только по частям: "чужой поток
// зовёт Server::stop()" (тесты выше, где остановку запускает поток теста
// напрямую) и "sigwait просыпается на настоящий SIGTERM"
// (tests/signal_handler_tests.cpp, где обработчик — обычная функция без
// Server). Этот тест сводит обе половины вместе: настоящий SIGTERM, дошедший
// до sigwait в сигнальном потоке, приводит к вызову Server::stop() из этого
// потока, а не из потока теста, и Server, живущий в своём io-потоке,
// действительно останавливается.
//
// Живой клиент с завершённым обменом PING/PONG (а не просто открытое
// соединение) обязателен по тому же доводу, что и в
// StopDrainsHangingSessionWithoutDeadlock выше: без него сессия ничего не
// держит на чтении, и остановка без обхода реестра сессий тоже была бы
// зелёной — тест проверял бы пустую остановку, а не drain.
TEST(NetworkTest, SignalHandlerStopsServerOnRealSigterm) {
    SignalMaskGuard maskGuard;
    ASSERT_TRUE(SignalHandler::blockSignals());

    CommandProcessor processor;
    RequestRouter router(processor, nullptr);
    ServerConfig config{"127.0.0.1", 0, 4096};
    boost::asio::io_context ioContext;
    Server server(ioContext, config, router);
    server.start();

    std::thread ioThread([&ioContext] {
        try {
            ioContext.run();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "io_context::run() threw: " << e.what();
        }
    });

    Client client(4096);
    client.connect("127.0.0.1", server.port());
    const nlohmann::json pong = client.request(nlohmann::json::parse(R"({"type":"PING"})"));
    // EXPECT, а не ASSERT: io-поток уже запущен, и досрочный возврат из теста
    // оставил бы его неприсоединённым — это std::terminate вместо падения.
    EXPECT_EQ(pong.at("status"), "OK");

    // Сессия сейчас висит на чтении следующего кадра — то незавершённое
    // чтение, которое и обязана закрыть остановка, пришедшая по сигналу.
    std::optional<SignalHandler> handler;
    handler.emplace([&server] { server.stop(); });

    EXPECT_EQ(::kill(::getpid(), SIGTERM), 0);

    constexpr std::chrono::seconds kTimeout(5);

    std::promise<void> stopped;
    std::future<void> stoppedFuture = stopped.get_future();
    std::thread joiner([&ioThread, &stopped] {
        ioThread.join();
        stopped.set_value();
    });

    if (stoppedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "io_context::run() не вернулась за отведённое время"
                          " после настоящего SIGTERM";
        // Присоединять поток, застрявший в незавершившейся run(), нельзя —
        // это и есть зависание, от которого тест обязан отличаться падением
        // (docs/task4/01-service-lifecycle.md, раздел 7.2, пункт 5), а не
        // попыткой join() заблокированного потока.
        std::fflush(nullptr);
        std::_Exit(1);
    }
    joiner.join();

    // Разрушение SignalHandler (SIGUSR1 + join сигнального потока) —
    // отдельная возможность зависнуть, не связанная с остановкой Server, и
    // ограничена по времени тем же приёмом, что и в
    // tests/signal_handler_tests.cpp.
    std::promise<void> destroyed;
    std::future<void> destroyedFuture = destroyed.get_future();
    std::thread destroyer([&handler, &destroyed] {
        handler.reset();
        destroyed.set_value();
    });

    if (destroyedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "SignalHandler не разрушился (SIGUSR1 + join) за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    destroyer.join();
}
