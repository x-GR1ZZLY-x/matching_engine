#include <gtest/gtest.h>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "command_processor.hpp"
#include "pg_connection.hpp"
#include "pg_result.hpp"
#include "request_router.hpp"
#include "test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

// RequestRouter вызывается напрямую, без сокета: его вход — уже отделённая
// кодеком полезная нагрузка одного кадра, выход — RouteResult (payload +
// fatal), поэтому проверять его можно обычными unit-тестами. connection
// передаётся явным nullptr: у конструктора нет умолчания — вырожденная
// конфигурация без БД обязана быть видна на месте вызова, а эти тесты не
// отправляют ADD/CANCEL/MODIFY, где соединение понадобилось бы.

TEST(RequestRouterTest, PingReturnsExactlyTwoFields) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    const nlohmann::json response =
        nlohmann::json::parse(router.handle(R"({"type":"PING"})").payload);

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("result"), "PONG");
    EXPECT_EQ(response.size(), 2u);
}

// Раздел 3.5 контракта: command_id возвращается эхом, если запрос
// разобрался в объект и содержит его строковым значением, — даже когда
// сама команда неизвестна. "FROB" — заведомо неизвестный тип: ADD
// маршрутизируется в обработку команды предметной области и эту ветку
// больше не проверяет.
TEST(RequestRouterTest, UnknownCommandTypeEchoesCommandId) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    const nlohmann::json response =
        nlohmann::json::parse(router.handle(R"({"type":"FROB","command_id":"cmd-1"})").payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_EQ(response.at("command_id"), "cmd-1");
}

// Битый JSON не разбирается в объект вовсе, поэтому command_id извлечь
// неоткуда — эха нет, даже если в исходном тексте что-то похожее на него
// присутствует.
TEST(RequestRouterTest, UnparsableJsonHasNoCommandIdEcho) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    const nlohmann::json response = nlohmann::json::parse(
        router.handle(R"(not json at all "command_id":"cmd-1")").payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_FALSE(response.contains("command_id"));
}

// Ответ об ошибке обязан оставаться
// маленьким независимо от размера запроса, иначе для любого
// max_message_size найдётся допустимый запрос, ответ на который лимит
// превысит. Неправдоподобно длинный command_id отвергается ещё до
// маршрутизации по типу команды (раздел 3.1/3.5 контракта) — поэтому тип
// здесь заведомо неизвестен ("FROB"), а не ADD: проверяется именно предел
// длины command_id, а не путь разбора команды.
TEST(RequestRouterTest, ImplausiblyLongCommandIdIsNotEchoed) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);
    const std::string hugeCommandId(10000, 'x');

    nlohmann::json request;
    request["type"] = "FROB";
    request["command_id"] = hugeCommandId;

    const nlohmann::json response = nlohmann::json::parse(router.handle(request.dump()).payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_FALSE(response.contains("command_id"));
}

// type, в отличие от command_id, — текст для человека внутри message, а не
// идентификатор: он усекается с многоточием вместо того, чтобы раздувать
// ответ до размера, сопоставимого с запросом.
TEST(RequestRouterTest, ImplausiblyLongTypeIsTruncatedInMessage) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);
    const std::string hugeType(10000, 'y');

    nlohmann::json request;
    request["type"] = hugeType;

    const std::string responsePayload = router.handle(request.dump()).payload;
    const nlohmann::json response = nlohmann::json::parse(responsePayload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_LT(responsePayload.size(), 500u);
    EXPECT_NE(response.at("message").get<std::string>().find("..."), std::string::npos);
}

// Замечание ревью: усечение по байтам может разрезать многобайтовую
// UTF-8-последовательность пополам, и nlohmann::json::dump() бросает
// json::type_error на невалидном байте. "€" (U+20AC) — три байта; 30
// повторов дают 90 байт, и граница усечения kMaxTypeInMessageLength (64
// байта, request_router.cpp) приходится ровно на середину 22-го символа.
// Двухбайтовая кириллица этот дефект не ловит: 64 делится на 2 нацело, и
// граница всегда попадала бы между символами — поэтому здесь трёхбайтовый
// символ, а не любой многобайтовый.
TEST(RequestRouterTest, ImplausiblyLongMultibyteTypeIsTruncatedOnCodepointBoundary) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    std::string hugeType;
    for (int i = 0; i < 30; ++i) {
        hugeType += "\xE2\x82\xAC";
    }

    nlohmann::json request;
    request["type"] = hugeType;

    // До исправления здесь бросало nlohmann::json::type_error ("invalid
    // UTF-8 byte") из ResponseSerializer::error() — и в Session это
    // разрывало бы соединение клиента, отправившего вполне безобидный
    // непопулярный тип команды.
    const std::string responsePayload = router.handle(request.dump()).payload;
    const nlohmann::json response = nlohmann::json::parse(responsePayload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_NE(response.at("message").get<std::string>().find("..."), std::string::npos);
}

// Замечание ревью второго круга, п.3: усечение по границе UTF-8-символа
// было покрыто тестом только на пути "type" (см. тест выше). Путь "message"
// использует ту же ResponseSerializer::truncateUtf8, но через другой
// источник текста: "order_type" отдаёт пользовательский текст напрямую в
// ParseError ("Unknown order_type: " + orderType, command_parser.cpp), и
// граница усечения kMaxMessageLength (512 байт) так же может прийтись на
// середину многобайтового символа. Префикс "Unknown order_type: " — 20
// байт; ведущий "X" сдвигает фазу так, чтобы граница 512 действительно
// прошлась по многобайтовому символу (без сдвига 512-20 делится на 3
// нацело, и граница всегда попадала бы между символами — тот же эффект,
// что и с чётным числом байт в тесте на "type" выше). 170 повторов "€" (3
// байта) после однобайтового префикса дают байт с индексом 512 (первый
// исключаемый) внутри кодовой последовательности "€" (продолжение,
// 10xxxxxx).
TEST(RequestRouterTest, ImplausiblyLongMultibyteOrderTypeIsTruncatedOnCodepointBoundaryInMessage) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    std::string hugeOrderType = "X";
    for (int i = 0; i < 170; ++i) {
        hugeOrderType += "\xE2\x82\xAC";
    }

    nlohmann::json request;
    request["type"] = "ADD";
    request["order_type"] = hugeOrderType;

    // До исправления границы усечения в message() этот путь не был
    // защищён отдельным тестом: тот же класс отказа (json::type_error из
    // dump() на невалидном UTF-8 хвосте), что уже закрыт для "type", мог
    // остаться непокрытым для "message".
    const std::string responsePayload = router.handle(request.dump()).payload;
    const nlohmann::json response = nlohmann::json::parse(responsePayload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_NE(response.at("message").get<std::string>().find("..."), std::string::npos);
}

// Конструктор без соединения к БД — это вырожденная, но легитимная
// конфигурация (тесты формата кадра), и она обязана быть видна на месте
// вызова, а не спрятана за умолчанием. Ветка тоже обязана логировать
// происходящее, а не отвечать молча.
TEST(RequestRouterTest, StateChangingCommandWithoutConnectionReturnsInternalError) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = 1;
    add["side"] = "BUY";
    add["price"] = 100;
    add["quantity"] = 1;
    add["command_id"] = "cmd-no-db";

    const RouteResult result = router.handle(add.dump());
    const nlohmann::json response = nlohmann::json::parse(result.payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INTERNAL_ERROR");
    EXPECT_FALSE(result.fatal);
}

// Критерий раздела 3.5: значения, не проходящие проверку предметной
// области (здесь — неположительная цена), получают код INVALID_ORDER, а не
// общий INVALID_REQUEST.
TEST(RequestRouterTest, NonPositivePriceReturnsInvalidOrder) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = 1;
    add["side"] = "BUY";
    add["price"] = -5;
    add["quantity"] = 1;
    add["command_id"] = "cmd-bad-price";

    const nlohmann::json response = nlohmann::json::parse(router.handle(add.dump()).payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_ORDER");
    EXPECT_EQ(response.at("command_id"), "cmd-bad-price");
}

// PRINT не участвует в идемпотентности
// и не требует command_id, но раздел 2 контракта требует вернуть его эхом,
// если клиент всё же его прислал — тем же правилом, что и для остальных
// неизменяющих команд.
TEST(RequestRouterTest, PrintEchoesCommandIdWhenProvided) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    nlohmann::json print;
    print["type"] = "PRINT";
    print["command_id"] = "cmd-print-1";

    const nlohmann::json response = nlohmann::json::parse(router.handle(print.dump()).payload);

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("command_id"), "cmd-print-1");
}

// HEALTH без соединения к БД (docs/task4/02-network-protocol.md, раздел
// 3.3) — connection_ == nullptr, поэтому database обязано быть "DOWN", а не
// "CONNECTED": буквенное значение по факту отсутствия указателя, а не
// опрос несуществующего соединения.
TEST(RequestRouterTest, HealthWithoutConnectionReportsDatabaseDown) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    const nlohmann::json response =
        nlohmann::json::parse(router.handle(R"({"type":"HEALTH"})").payload);

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("database"), "DOWN");
    EXPECT_EQ(response.at("engine"), "READY");
}

// HEALTH с живым соединением — противоположная ветка buildHealth():
// PgConnection::isConnected() истинен, database обязано быть "CONNECTED".
TEST(RequestRouterTest, HealthWithLiveConnectionReportsDatabaseConnected) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    CommandProcessor processor;
    RequestRouter router(processor, &*connOpt);

    const nlohmann::json response =
        nlohmann::json::parse(router.handle(R"({"type":"HEALTH"})").payload);

    EXPECT_EQ(response.at("database"), "CONNECTED");
}

// Полезная нагрузка — синтаксически валидный JSON, но не объект (число
// верхнего уровня). Отдельная ветка от UnparsableJsonHasNoCommandIdEcho
// выше: там json::parse бросает, здесь разбор проходит успешно, но
// проваливается проверка is_object()/contains("type").
TEST(RequestRouterTest, TopLevelJsonNotObjectReturnsInvalidRequest) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    const nlohmann::json response = nlohmann::json::parse(router.handle("42").payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
}

// Объект без поля "type" вовсе — та же ветка (contains("type") ложно), но
// другой конкретный сценарий, чем "type" неверного типа ниже.
TEST(RequestRouterTest, ObjectWithoutTypeFieldReturnsInvalidRequest) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    const nlohmann::json response = nlohmann::json::parse(router.handle("{}").payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
}

// "type" присутствует, но не строка — третья вариация условия на строке
// 212 request_router.cpp (is_object && contains(type) && is_string).
TEST(RequestRouterTest, NonStringTypeFieldReturnsInvalidRequest) {
    CommandProcessor processor;
    RequestRouter router(processor, nullptr);

    const nlohmann::json response =
        nlohmann::json::parse(router.handle(R"({"type":5})").payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
}

// Единственный путь handleDomainCommand(), до сих пор ни разу не пройденный
// целиком с реальным соединением: успешная запись команды даёт
// ResponseSerializer::success() с orderId и числом сделок в RouteResult
// (route.orderId/route.tradesCount), а не только payload.
TEST(RequestRouterTest, SuccessfulAddCommandReturnsSuccessResponse) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kOrderId = 902200001;
    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("router-success-add")});

    CommandProcessor processor;
    RequestRouter router(processor, &conn);

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = kOrderId;
    add["side"] = "BUY";
    add["price"] = 100;
    add["quantity"] = 3;
    add["command_id"] = "router-success-add";

    const RouteResult result = router.handle(add.dump());
    const nlohmann::json response = nlohmann::json::parse(result.payload);

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(result.orderId, kOrderId);
    EXPECT_EQ(result.tradesCount, 0u);

    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("router-success-add")});
}

// ADD с order_id уже открытой в книге заявки — MatchingEngine бросает
// DuplicateOrderError, RequestRouter обязан отдать именно код
// DUPLICATE_ORDER (отдельная ветка от общего INVALID_REQUEST).
TEST(RequestRouterTest, AddWithDuplicateOrderIdReturnsDuplicateOrder) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kOrderId = 902200002;
    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("router-dup-1"), std::optional<std::string>("router-dup-2")});

    CommandProcessor processor;
    RequestRouter router(processor, &conn);

    nlohmann::json first;
    first["type"] = "ADD";
    first["order_id"] = kOrderId;
    first["side"] = "BUY";
    first["price"] = 100;
    first["quantity"] = 3;
    first["command_id"] = "router-dup-1";
    router.handle(first.dump());

    nlohmann::json second;
    second["type"] = "ADD";
    second["order_id"] = kOrderId;
    second["side"] = "BUY";
    second["price"] = 100;
    second["quantity"] = 5;
    second["command_id"] = "router-dup-2";
    const nlohmann::json response = nlohmann::json::parse(router.handle(second.dump()).payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "DUPLICATE_ORDER");

    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("router-dup-1"), std::optional<std::string>("router-dup-2")});
}

// MODIFY через RequestRouter — extractOrderId() до сих пор проходил только
// ветку AddCommand/CancelCommand (тесты выше); ModifyCommand — отдельная
// ветка dynamic_cast в extractOrderId (request_router.cpp:56), не
// пройденная ни разу.
TEST(RequestRouterTest, SuccessfulModifyCommandReturnsSuccessResponse) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kOrderId = 902200003;
    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("router-modify-add"),
            std::optional<std::string>("router-modify-modify")});

    CommandProcessor processor;
    RequestRouter router(processor, &conn);

    nlohmann::json add;
    add["type"] = "ADD";
    add["order_id"] = kOrderId;
    add["side"] = "BUY";
    add["price"] = 100;
    add["quantity"] = 5;
    add["command_id"] = "router-modify-add";
    router.handle(add.dump());

    nlohmann::json modify;
    modify["type"] = "MODIFY";
    modify["order_id"] = kOrderId;
    modify["price"] = 100;
    modify["quantity"] = 2;
    modify["command_id"] = "router-modify-modify";

    const RouteResult result = router.handle(modify.dump());
    const nlohmann::json response = nlohmann::json::parse(result.payload);

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(result.orderId, kOrderId);

    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("router-modify-add"),
            std::optional<std::string>("router-modify-modify")});
}

// MARKET ADD (поле order_type) через RequestRouter — extractOrderId()
// ветка MarketAddCommand, тоже ни разу не пройденная. MARKET-заявка без
// встречной ликвидности не попадает в книгу (доменное правило проекта),
// но команда всё равно успешно выполняется и сохраняется.
TEST(RequestRouterTest, SuccessfulMarketAddCommandReturnsSuccessResponse) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kOrderId = 902200004;
    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("router-market-add")});

    CommandProcessor processor;
    RequestRouter router(processor, &conn);

    nlohmann::json marketAdd;
    marketAdd["type"] = "ADD";
    marketAdd["order_type"] = "MARKET";
    marketAdd["order_id"] = kOrderId;
    marketAdd["side"] = "BUY";
    marketAdd["quantity"] = 5;
    marketAdd["command_id"] = "router-market-add";

    const RouteResult result = router.handle(marketAdd.dump());
    const nlohmann::json response = nlohmann::json::parse(result.payload);

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(result.orderId, kOrderId);
    EXPECT_EQ(result.tradesCount, 0u);

    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("router-market-add")});
}

// CANCEL на несуществующей заявке — MatchingEngine бросает OrderBookError,
// RequestRouter обязан отдать ORDER_NOT_FOUND (отдельная ветка от
// DUPLICATE_ORDER выше и от общего INVALID_REQUEST).
TEST(RequestRouterTest, CancelNonexistentOrderReturnsOrderNotFound) {
    auto connOpt = tryConnect();
    if (!connOpt) {
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("router-cancel-missing")});

    CommandProcessor processor;
    RequestRouter router(processor, &conn);

    nlohmann::json cancel;
    cancel["type"] = "CANCEL";
    cancel["order_id"] = 902200099;
    cancel["command_id"] = "router-cancel-missing";

    const nlohmann::json response = nlohmann::json::parse(router.handle(cancel.dump()).payload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "ORDER_NOT_FOUND");

    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("router-cancel-missing")});
}
