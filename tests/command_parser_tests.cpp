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