#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "request_router.hpp"

using namespace matching_engine;

// RequestRouter вызывается напрямую, без сокета (docs/hw4/tasks/task-04.md,
// "файл тестов на модуль"): его вход — уже отделённая кодеком полезная
// нагрузка одного кадра, выход — строка JSON-ответа.

TEST(RequestRouterTest, PingReturnsExactlyTwoFields) {
    RequestRouter router;

    const nlohmann::json response = nlohmann::json::parse(router.handle(R"({"type":"PING"})"));

    EXPECT_EQ(response.at("status"), "OK");
    EXPECT_EQ(response.at("result"), "PONG");
    EXPECT_EQ(response.size(), 2u);
}

// Раздел 3.5 контракта: command_id возвращается эхом, если запрос
// разобрался в объект и содержит его строковым значением, — даже когда
// сама команда неизвестна.
TEST(RequestRouterTest, UnknownCommandTypeEchoesCommandId) {
    RequestRouter router;

    const nlohmann::json response =
        nlohmann::json::parse(router.handle(R"({"type":"ADD","command_id":"cmd-1"})"));

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_EQ(response.at("command_id"), "cmd-1");
}

// Битый JSON не разбирается в объект вовсе, поэтому command_id извлечь
// неоткуда — эха нет, даже если в исходном тексте что-то похожее на него
// присутствует.
TEST(RequestRouterTest, UnparsableJsonHasNoCommandIdEcho) {
    RequestRouter router;

    const nlohmann::json response =
        nlohmann::json::parse(router.handle(R"(not json at all "command_id":"cmd-1")"));

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_FALSE(response.contains("command_id"));
}

// Второй круг ревью задачи 04: ответ об ошибке обязан оставаться
// маленьким независимо от размера запроса, иначе для любого
// max_message_size найдётся допустимый запрос, ответ на который лимит
// превысит. Неправдоподобно длинный command_id в ответ не попадает вовсе —
// усечённый идентификатор клиент не сопоставит со своим запросом.
TEST(RequestRouterTest, ImplausiblyLongCommandIdIsNotEchoed) {
    RequestRouter router;
    const std::string hugeCommandId(10000, 'x');

    nlohmann::json request;
    request["type"] = "ADD";
    request["command_id"] = hugeCommandId;

    const nlohmann::json response = nlohmann::json::parse(router.handle(request.dump()));

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_FALSE(response.contains("command_id"));
}

// type, в отличие от command_id, — текст для человека внутри message, а не
// идентификатор: он усекается с многоточием вместо того, чтобы раздувать
// ответ до размера, сопоставимого с запросом.
TEST(RequestRouterTest, ImplausiblyLongTypeIsTruncatedInMessage) {
    RequestRouter router;
    const std::string hugeType(10000, 'y');

    nlohmann::json request;
    request["type"] = hugeType;

    const std::string responsePayload = router.handle(request.dump());
    const nlohmann::json response = nlohmann::json::parse(responsePayload);

    EXPECT_EQ(response.at("status"), "ERROR");
    EXPECT_EQ(response.at("error"), "INVALID_REQUEST");
    EXPECT_LT(responsePayload.size(), 500u);
    EXPECT_NE(response.at("message").get<std::string>().find("..."), std::string::npos);
}
