#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "command_processor.hpp"
#include "request_router.hpp"

using namespace matching_engine;

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
