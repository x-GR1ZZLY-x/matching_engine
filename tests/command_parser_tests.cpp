#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "command_parser.hpp"

using namespace matching_engine;

static nlohmann::json fromString(const std::string& s)
{
    return nlohmann::json::parse(s);
}

// ─── ADD ──────────────────────────────────────────────────────────────────────

TEST(CommandParserTest, ParseValidAddBuy)
{
    CommandParser parser;
    auto cmd = parser.parse(fromString(
        R"({"type":"ADD","id":1,"side":"BUY","price":100,"quantity":10})"
    ));

    ASSERT_EQ(cmd->type_, CommandType::Add);
    const auto& add = static_cast<const AddCommand&>(*cmd);
    EXPECT_EQ(add.id_,       1);
    EXPECT_EQ(add.side_,     Side::Buy);
    EXPECT_EQ(add.price_,    100);
    EXPECT_EQ(add.quantity_, 10);
}

TEST(CommandParserTest, ParseValidAddSell)
{
    CommandParser parser;
    auto cmd = parser.parse(fromString(
        R"({"type":"ADD","id":2,"side":"SELL","price":200,"quantity":5})"
    ));

    const auto& add = static_cast<const AddCommand&>(*cmd);
    EXPECT_EQ(add.side_,  Side::Sell);
    EXPECT_EQ(add.price_, 200);
}

// ─── CANCEL ───────────────────────────────────────────────────────────────────

TEST(CommandParserTest, ParseValidCancel)
{
    CommandParser parser;
    auto cmd = parser.parse(fromString(R"({"type":"CANCEL","id":42})"));

    ASSERT_EQ(cmd->type_, CommandType::Cancel);
    const auto& cancel = static_cast<const CancelCommand&>(*cmd);
    EXPECT_EQ(cancel.id_, 42);
}

// ─── PRINT ────────────────────────────────────────────────────────────────────

TEST(CommandParserTest, ParseValidPrint)
{
    CommandParser parser;
    auto cmd = parser.parse(fromString(R"({"type":"PRINT"})"));
    EXPECT_EQ(cmd->type_, CommandType::Print);
}

// ─── Неизвестный тип команды ──────────────────────────────────────────────────

TEST(CommandParserTest, UnknownCommandTypeThrows)
{
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"DELETE","id":1})")),
        ParseError
    );
}

TEST(CommandParserTest, MissingTypeFieldThrows)
{
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"id":1,"side":"BUY","price":100,"quantity":10})")),
        ParseError
    );
}

// ─── Отсутствующие поля в ADD ─────────────────────────────────────────────────

TEST(CommandParserTest, AddMissingIdThrows)
{
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","side":"BUY","price":100,"quantity":10})")),
        ParseError
    );
}

TEST(CommandParserTest, AddMissingSideThrows)
{
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"price":100,"quantity":10})")),
        ParseError
    );
}

// ─── Неправильный тип поля ────────────────────────────────────────────────────

TEST(CommandParserTest, AddPriceAsStringThrows)
{
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(
            R"({"type":"ADD","id":1,"side":"BUY","price":"hundred","quantity":10})"
        )),
        ParseError
    );
}

// ─── Неправильная сторона заявки ──────────────────────────────────────────────

TEST(CommandParserTest, AddUnknownSideThrows)
{
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(
            R"({"type":"ADD","id":1,"side":"HOLD","price":100,"quantity":10})"
        )),
        ParseError
    );
}

TEST(CommandParserTest, ParseMarketAdd) {
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "MARKET",
        "id": 5,
        "side": "BUY",
        "quantity": 10
    })");

    const auto cmd = parser.parse(json);
    ASSERT_NE(cmd, nullptr);

    const auto* market = dynamic_cast<const MarketAddCommand*>(cmd.get());
    ASSERT_NE(market, nullptr);
    EXPECT_EQ(market->id_,       5);
    EXPECT_EQ(market->side_,     Side::Buy);
    EXPECT_EQ(market->quantity_, 10);
}

TEST(CommandParserTest, ParseMarketAddMissingQuantity) {
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "MARKET",
        "id": 5,
        "side": "BUY"
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseUnknownOrderType) {
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "FOO",
        "id": 5,
        "side": "BUY",
        "quantity": 10
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseModify) {
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 10,
        "price": 105,
        "quantity": 20
    })");

    const auto cmd = parser.parse(json);
    ASSERT_NE(cmd, nullptr);

    const auto* modify = dynamic_cast<const ModifyCommand*>(cmd.get());
    ASSERT_NE(modify, nullptr);
    EXPECT_EQ(modify->id_,       10);
    EXPECT_EQ(modify->price_,    105);
    EXPECT_EQ(modify->quantity_, 20);
}

TEST(CommandParserTest, ParseModifyMissingPrice) {
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 10,
        "quantity": 20
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseModifyNegativePrice) {
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 10,
        "price": -5,
        "quantity": 20
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

// ─── command_id ─────────────────────────────────────────────────────────────
// Необязателен на уровне парсера (докстрока над Command::commandId_):
// существующие тесты выше подают JSON без command_id и по-прежнему успешно
// разбираются. Здесь — что поле подхватывается, когда оно есть, для любого
// типа команды, и что неверный тип поля отвергается.

TEST(CommandParserTest, ParseAddWithoutCommandIdLeavesItEmpty){
    CommandParser parser;
    auto cmd = parser.parse(fromString(
        R"({"type":"ADD","id":1,"side":"BUY","price":100,"quantity":10})"
    ));

    EXPECT_FALSE(cmd->commandId_.has_value());
}

TEST(CommandParserTest, ParseAddWithCommandIdSetsField){
    CommandParser parser;
    auto cmd = parser.parse(fromString(
        R"({"type":"ADD","id":1,"side":"BUY","price":100,"quantity":10,)"
        R"("command_id":"abc-123"})"
    ));

    ASSERT_TRUE(cmd->commandId_.has_value());
    EXPECT_EQ(*cmd->commandId_, "abc-123");
}

TEST(CommandParserTest, ParseCancelWithCommandIdSetsField){
    CommandParser parser;
    auto cmd = parser.parse(fromString(
        R"({"type":"CANCEL","id":1,"command_id":"cancel-1"})"
    ));

    ASSERT_TRUE(cmd->commandId_.has_value());
    EXPECT_EQ(*cmd->commandId_, "cancel-1");
}

TEST(CommandParserTest, ParsePrintWithoutCommandIdSucceeds){
    CommandParser parser;
    auto cmd = parser.parse(fromString(R"({"type":"PRINT"})"));

    EXPECT_EQ(cmd->type_, CommandType::Print);
    EXPECT_FALSE(cmd->commandId_.has_value());
}

TEST(CommandParserTest, CommandIdAsNumberThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(
            R"({"type":"ADD","id":1,"side":"BUY","price":100,"quantity":10,)"
            R"("command_id":123})"
        )),
        ParseError
    );
}

// ─── order_id / id — синоним идентификатора ────────────────────────────────
// requireOrderId предпочитает "order_id" и только при его отсутствии
// заглядывает в "id" (command_parser.cpp:39-44). Все тесты выше подают
// только "id" — ветка с "order_id" никогда не проходится. Без этого теста
// реализация, у которой сломан именно путь "order_id" (например,
// требующая "id" всегда), осталась бы незамеченной.

TEST(CommandParserTest, ParseAddAcceptsOrderIdFieldName){
    CommandParser parser;
    auto cmd = parser.parse(fromString(
        R"({"type":"ADD","order_id":7,"side":"BUY","price":100,"quantity":10})"
    ));

    const auto& add = static_cast<const AddCommand&>(*cmd);
    EXPECT_EQ(add.id_, 7);
}

TEST(CommandParserTest, ParseCancelAcceptsOrderIdFieldName){
    CommandParser parser;
    auto cmd = parser.parse(fromString(R"({"type":"CANCEL","order_id":9})"));

    const auto& cancel = static_cast<const CancelCommand&>(*cmd);
    EXPECT_EQ(cancel.id_, 9);
}

// ─── ADD: границы id/price/quantity ─────────────────────────────────────────
// Только отсутствие полей проверялось выше; здесь — присутствующие, но
// недопустимые значения (<=0), и отдельно от негативной ветки "side".

TEST(CommandParserTest, AddZeroIdThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":0,"side":"BUY","price":100,"quantity":10})")),
        ParseError
    );
}

TEST(CommandParserTest, AddNegativeIdThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":-1,"side":"BUY","price":100,"quantity":10})")),
        ParseError
    );
}

TEST(CommandParserTest, AddMissingPriceThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"side":"BUY","quantity":10})")),
        ParseError
    );
}

TEST(CommandParserTest, AddZeroPriceThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"side":"BUY","price":0,"quantity":10})")),
        InvalidOrderValueError
    );
}

TEST(CommandParserTest, AddNegativePriceThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"side":"BUY","price":-100,"quantity":10})")),
        InvalidOrderValueError
    );
}

TEST(CommandParserTest, AddMissingQuantityThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"side":"BUY","price":100})")),
        ParseError
    );
}

TEST(CommandParserTest, AddZeroQuantityThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"side":"BUY","price":100,"quantity":0})")),
        InvalidOrderValueError
    );
}

TEST(CommandParserTest, AddNegativeQuantityThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"side":"BUY","price":100,"quantity":-5})")),
        InvalidOrderValueError
    );
}

// requireString отвергает нестроковое значение — эта ветка (в отличие от
// requireInt) до сих пор проходилась только на "type"/"price", никогда на
// "side": AddUnknownSideThrows подаёт валидную строку с неизвестным
// значением, а не значение другого типа.
TEST(CommandParserTest, AddSideAsNumberThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"ADD","id":1,"side":1,"price":100,"quantity":10})")),
        ParseError
    );
}

// ─── CANCEL: границы id ──────────────────────────────────────────────────

TEST(CommandParserTest, CancelMissingIdThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"CANCEL"})")),
        ParseError
    );
}

TEST(CommandParserTest, CancelZeroIdThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"CANCEL","id":0})")),
        ParseError
    );
}

TEST(CommandParserTest, CancelNegativeIdThrows){
    CommandParser parser;
    EXPECT_THROW(
        parser.parse(fromString(R"({"type":"CANCEL","id":-3})")),
        ParseError
    );
}

// ─── ADD MARKET: границы и отсутствующие поля ───────────────────────────
// Ранее была проверена только нехватка quantity; здесь — остальные ветки
// parseMarketAdd (command_parser.cpp:116-132), полностью параллельные
// лимитной ветке, но со своим кодом и легко расходящиеся при правке одной
// из веток без другой.

TEST(CommandParserTest, ParseMarketAddMissingSideThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "MARKET",
        "id": 5,
        "quantity": 10
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseMarketAddInvalidSideThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "MARKET",
        "id": 5,
        "side": "HOLD",
        "quantity": 10
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseMarketAddZeroIdThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "MARKET",
        "id": 0,
        "side": "BUY",
        "quantity": 10
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseMarketAddZeroQuantityThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "MARKET",
        "id": 5,
        "side": "BUY",
        "quantity": 0
    })");

    EXPECT_THROW(parser.parse(json), InvalidOrderValueError);
}

TEST(CommandParserTest, ParseMarketAddNegativeQuantityThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "ADD",
        "order_type": "MARKET",
        "id": 5,
        "side": "BUY",
        "quantity": -1
    })");

    EXPECT_THROW(parser.parse(json), InvalidOrderValueError);
}

// ─── MODIFY: недостающие/непригодные поля ───────────────────────────────
// Ранее покрыты только отсутствие price и отрицательная price; остальные
// ветки parseModify (command_parser.cpp:134-144) не проходились ни разу.

TEST(CommandParserTest, ParseModifyMissingIdThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "price": 105,
        "quantity": 20
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseModifyZeroIdThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 0,
        "price": 105,
        "quantity": 20
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseModifyMissingQuantityThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 10,
        "price": 105
    })");

    EXPECT_THROW(parser.parse(json), ParseError);
}

TEST(CommandParserTest, ParseModifyZeroQuantityThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 10,
        "price": 105,
        "quantity": 0
    })");

    EXPECT_THROW(parser.parse(json), InvalidOrderValueError);
}

TEST(CommandParserTest, ParseModifyNegativeQuantityThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 10,
        "price": 105,
        "quantity": -20
    })");

    EXPECT_THROW(parser.parse(json), InvalidOrderValueError);
}

TEST(CommandParserTest, ParseModifyZeroPriceThrows){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "id": 10,
        "price": 0,
        "quantity": 20
    })");

    EXPECT_THROW(parser.parse(json), InvalidOrderValueError);
}

TEST(CommandParserTest, ParseModifyAcceptsOrderIdFieldName){
    CommandParser parser;
    const auto json = nlohmann::json::parse(R"({
        "type": "MODIFY",
        "order_id": 11,
        "price": 105,
        "quantity": 20
    })");

    const auto cmd = parser.parse(json);
    const auto* modify = dynamic_cast<const ModifyCommand*>(cmd.get());
    ASSERT_NE(modify, nullptr);
    EXPECT_EQ(modify->id_, 11);
}