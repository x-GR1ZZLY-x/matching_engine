#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "response_serializer.hpp"
#include "order.hpp"

using namespace matching_engine;

namespace {

ExecutionResult resultWithOneTrade(){
    ExecutionResult result;
    result.trades.emplace_back(1, 2, 100, 5);
    return result;
}

ExecutionResult resultWithoutTrades(){
    return ExecutionResult{};
}

}

// ─── success() ──────────────────────────────────────────────────────────

// Без сделок ответ содержит пустой массив "trades", а не отсутствующее
// поле и не null — ветка цикла по result.trades не выполняется ни разу, но
// поле обязано остаться корректным JSON-массивом.
TEST(ResponseSerializerTest, SuccessWithoutTradesProducesEmptyTradesArray){
    const auto json = nlohmann::json::parse(
        ResponseSerializer::success(std::nullopt, 7, resultWithoutTrades()));

    EXPECT_EQ(json.at("status"), "OK");
    EXPECT_EQ(json.at("order_id"), 7);
    ASSERT_TRUE(json.at("trades").is_array());
    EXPECT_TRUE(json.at("trades").empty());
    EXPECT_FALSE(json.contains("command_id"));
}

// Со сделкой массив "trades" заполняется одной записью с корректными
// сторонами/ценой/количеством — цикл по result.trades выполняется хотя бы
// раз и поля берутся именно из Trade, а не из аргументов success().
TEST(ResponseSerializerTest, SuccessWithTradeIncludesTradeFields){
    const auto json = nlohmann::json::parse(
        ResponseSerializer::success(std::nullopt, 7, resultWithOneTrade()));

    ASSERT_TRUE(json.at("trades").is_array());
    ASSERT_EQ(json.at("trades").size(), 1u);
    const auto& trade = json.at("trades").at(0);
    EXPECT_EQ(trade.at("buy_order_id"), 1);
    EXPECT_EQ(trade.at("sell_order_id"), 2);
    EXPECT_EQ(trade.at("price"), 100);
    EXPECT_EQ(trade.at("quantity"), 5);
}

// commandId присутствует и достаточно короток — putCommandIdIfEligible
// обязана его пропустить в ответ.
TEST(ResponseSerializerTest, SuccessWithEligibleCommandIdIncludesIt){
    const auto json = nlohmann::json::parse(
        ResponseSerializer::success(std::string("cmd-1"), 7, resultWithoutTrades()));

    ASSERT_TRUE(json.contains("command_id"));
    EXPECT_EQ(json.at("command_id"), "cmd-1");
}

// commandId длиннее kMaxCommandIdLength — эхо не попадает в ответ вовсе:
// без этой проверки раздутый command_id мог бы неограниченно увеличивать
// размер успешного ответа так же, как и ответа об ошибке.
TEST(ResponseSerializerTest, SuccessWithOverlongCommandIdOmitsIt){
    const std::string overlong(ResponseSerializer::kMaxCommandIdLength + 1, 'a');

    const auto json = nlohmann::json::parse(
        ResponseSerializer::success(overlong, 7, resultWithoutTrades()));

    EXPECT_FALSE(json.contains("command_id"));
}

// Ровно граничная длина command_id — ещё допустима (граница "<=", а не "<").
TEST(ResponseSerializerTest, SuccessWithCommandIdAtExactLimitIncludesIt){
    const std::string atLimit(ResponseSerializer::kMaxCommandIdLength, 'b');

    const auto json = nlohmann::json::parse(
        ResponseSerializer::success(atLimit, 7, resultWithoutTrades()));

    ASSERT_TRUE(json.contains("command_id"));
    EXPECT_EQ(json.at("command_id"), atLimit);
}

// ─── error() ────────────────────────────────────────────────────────────

TEST(ResponseSerializerTest, ErrorWithoutCommandIdOmitsField){
    const auto json = nlohmann::json::parse(
        ResponseSerializer::error(std::nullopt, "INVALID_REQUEST", "bad"));

    EXPECT_EQ(json.at("status"), "ERROR");
    EXPECT_EQ(json.at("error"), "INVALID_REQUEST");
    EXPECT_EQ(json.at("message"), "bad");
    EXPECT_FALSE(json.contains("command_id"));
}

TEST(ResponseSerializerTest, ErrorWithCommandIdIncludesIt){
    const auto json = nlohmann::json::parse(
        ResponseSerializer::error(std::string("cmd-2"), "NOT_FOUND", "no such order"));

    ASSERT_TRUE(json.contains("command_id"));
    EXPECT_EQ(json.at("command_id"), "cmd-2");
}

// Сообщение длиннее kMaxMessageLength обязано быть усечено с добавлением
// "..." — без truncateUtf8 длинный текст исключения раздувал бы ответ без
// предела.
TEST(ResponseSerializerTest, ErrorTruncatesOverlongMessage){
    const std::string longMessage(ResponseSerializer::kMaxMessageLength + 50, 'x');

    const auto json = nlohmann::json::parse(
        ResponseSerializer::error(std::nullopt, "INVALID_ORDER", longMessage));

    const std::string message = json.at("message").get<std::string>();
    EXPECT_LT(message.size(), longMessage.size());
    EXPECT_EQ(message.substr(message.size() - 3), "...");
}

// Сообщение короче предела не усекается вовсе — ветка "без обрезки" в
// truncateUtf8, вызванная через error().
TEST(ResponseSerializerTest, ErrorDoesNotTruncateShortMessage){
    const auto json = nlohmann::json::parse(
        ResponseSerializer::error(std::nullopt, "INVALID_ORDER", "short"));

    EXPECT_EQ(json.at("message"), "short");
}

// ─── truncateUtf8() ─────────────────────────────────────────────────────

// Граница усечения приходится ровно на однобайтовый ASCII-символ — цикл
// отступа назад не должен выполниться ни разу.
TEST(ResponseSerializerTest, TruncateUtf8OnAsciiBoundaryAddsEllipsisAtExactCut){
    const std::string value = "abcdef";

    const std::string truncated = ResponseSerializer::truncateUtf8(value, 4);

    EXPECT_EQ(truncated, "abcd...");
}

// Значение короче предела возвращается без изменений — ветка "value.size()
// <= maxLength" в truncateUtf8.
TEST(ResponseSerializerTest, TruncateUtf8WithinLimitReturnsUnchanged){
    const std::string value = "abc";

    EXPECT_EQ(ResponseSerializer::truncateUtf8(value, 10), value);
}

// Граница усечения попадает внутрь двухбайтовой UTF-8 последовательности
// ("ё" — 0xD1 0x91): цикл отступа назад обязан сдвинуть границу на начало
// этой последовательности и отбросить её целиком, а не оставить в строке
// одинокий байт-продолжение, на котором dump() иначе бросил бы исключение.
TEST(ResponseSerializerTest, TruncateUtf8OnMultibyteBoundaryDropsWholeSequence){
    const std::string value = "a\xD1\x91z"; // 'a', 'ё' (2 байта), 'z'

    // maxLength = 2 указывает ровно на второй байт "ё" (продолжение).
    const std::string truncated = ResponseSerializer::truncateUtf8(value, 2);

    EXPECT_EQ(truncated, "a...");
}

// Граница усечения попадает внутрь трёхбайтовой последовательности
// ("€" — 0xE2 0x82 0xAC): цикл отступа назад обязан сдвинуться на два байта
// назад (два байта-продолжения подряд), а не остановиться после первого
// шага, как в двухбайтовом случае выше.
TEST(ResponseSerializerTest, TruncateUtf8OnThreeByteSequenceBoundaryDropsWholeSequence){
    const std::string value = "a\xE2\x82\xACz"; // 'a', '€' (3 байта), 'z'

    // maxLength = 3 указывает на третий байт "€" (второй байт-продолжение).
    const std::string truncated = ResponseSerializer::truncateUtf8(value, 3);

    EXPECT_EQ(truncated, "a...");
}

// Четырёхбайтовая последовательность в самом начале строки, а граница
// усечения указывает на её последний байт: цикл отступа назад должен дойти
// ровно до позиции 0 и остановиться там через условие "cut > 0" (а не через
// проверку байта на продолжение) — до этого теста ветка "cut стал равен
// нулю" ни разу не срабатывала ни в одном тесте файла.
TEST(ResponseSerializerTest, TruncateUtf8OnFourByteSequenceAtStartStopsAtZero){
    const std::string value = "\xF0\x9F\x98\x80z"; // '😀' (4 байта) + 'z'

    const std::string truncated = ResponseSerializer::truncateUtf8(value, 3);

    EXPECT_EQ(truncated, "...");
}

// ─── maxErrorResponseSize() ─────────────────────────────────────────────

// Худший случай обязан совпадать с реальным вызовом error() на предельных
// входах, а не с произвольной константой: пересобираем его тем же образом,
// каким это делает сама функция, и сравниваем длины.
TEST(ResponseSerializerTest, MaxErrorResponseSizeMatchesWorstCaseCall){
    const std::string worstCaseCommandId(ResponseSerializer::kMaxCommandIdLength,
        static_cast<char>(1));
    const std::string worstCaseMessage(ResponseSerializer::kMaxMessageLength + 1,
        static_cast<char>(1));

    const std::size_t expected =
        ResponseSerializer::error(worstCaseCommandId, "RESPONSE_TOO_LARGE", worstCaseMessage)
            .size();

    EXPECT_EQ(ResponseSerializer::maxErrorResponseSize(), expected);
}

// Любой обычный ответ об ошибке обязан помещаться в объявленный "худший
// случай" — иначе граница не была бы верхней.
TEST(ResponseSerializerTest, MaxErrorResponseSizeBoundsOrdinaryError){
    const std::string ordinary =
        ResponseSerializer::error(std::string("cmd-1"), "INVALID_ORDER", "Price must be positive");

    EXPECT_LE(ordinary.size(), ResponseSerializer::maxErrorResponseSize());
}

// ─── printBook() ────────────────────────────────────────────────────────

TEST(ResponseSerializerTest, PrintBookOnEmptyBookProducesEmptyArrays){
    const OrderBook book;

    const auto json = nlohmann::json::parse(ResponseSerializer::printBook(book));

    EXPECT_EQ(json.at("status"), "OK");
    ASSERT_TRUE(json.at("result").at("buy").is_array());
    ASSERT_TRUE(json.at("result").at("sell").is_array());
    EXPECT_TRUE(json.at("result").at("buy").empty());
    EXPECT_TRUE(json.at("result").at("sell").empty());
    EXPECT_FALSE(json.contains("command_id"));
}

// Книга с заявками по обе стороны — циклы по buyOrders()/sellOrders()
// выполняются, и поля записи (order_id/price/quantity) берутся из самой
// заявки.
TEST(ResponseSerializerTest, PrintBookWithOrdersOnBothSidesIncludesThem){
    OrderBook book;
    book.addOrder(std::make_shared<Order>(1, Side::Buy, 100, 10));
    book.addOrder(std::make_shared<Order>(2, Side::Sell, 200, 4));

    const auto json = nlohmann::json::parse(
        ResponseSerializer::printBook(book, std::string("cmd-3")));

    ASSERT_TRUE(json.contains("command_id"));
    EXPECT_EQ(json.at("command_id"), "cmd-3");

    ASSERT_EQ(json.at("result").at("buy").size(), 1u);
    const auto& buyEntry = json.at("result").at("buy").at(0);
    EXPECT_EQ(buyEntry.at("order_id"), 1);
    EXPECT_EQ(buyEntry.at("price"), 100);
    EXPECT_EQ(buyEntry.at("quantity"), 10);

    ASSERT_EQ(json.at("result").at("sell").size(), 1u);
    const auto& sellEntry = json.at("result").at("sell").at(0);
    EXPECT_EQ(sellEntry.at("order_id"), 2);
    EXPECT_EQ(sellEntry.at("price"), 200);
    EXPECT_EQ(sellEntry.at("quantity"), 4);
}
