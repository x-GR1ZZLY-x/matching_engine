#include <gtest/gtest.h>
#include "order.hpp"

using namespace matching_engine;

// ─── Создание корректной заявки ───────────────────────────────────────────────

TEST(OrderTest, CreateValidBuyOrder)
{
    Order order(1, Side::Buy, 100, 10);

    EXPECT_EQ(order.getId(),       1);
    EXPECT_EQ(order.getSide(),     Side::Buy);
    EXPECT_EQ(order.getPrice(),    100);
    EXPECT_EQ(order.getQuantity(), 10);
    EXPECT_FALSE(order.isFilled());
}

TEST(OrderTest, CreateValidSellOrder)
{
    Order order(42, Side::Sell, 250, 5);

    EXPECT_EQ(order.getId(),       42);
    EXPECT_EQ(order.getSide(),     Side::Sell);
    EXPECT_EQ(order.getPrice(),    250);
    EXPECT_EQ(order.getQuantity(), 5);
}

// ─── Невалидные цены ──────────────────────────────────────────────────────────

TEST(OrderTest, ZeroPriceThrows)
{
    EXPECT_THROW(Order(1, Side::Buy, 0, 10), OrderError);
}

TEST(OrderTest, NegativePriceThrows)
{
    EXPECT_THROW(Order(1, Side::Buy, -5, 10), OrderError);
}

// ─── Невалидные количества ────────────────────────────────────────────────────

TEST(OrderTest, ZeroQuantityThrows)
{
    EXPECT_THROW(Order(1, Side::Buy, 100, 0), OrderError);
}

TEST(OrderTest, NegativeQuantityThrows)
{
    EXPECT_THROW(Order(1, Side::Buy, 100, -3), OrderError);
}

// ─── Частичное и полное исполнение ───────────────────────────────────────────

TEST(OrderTest, PartialFill)
{
    Order order(1, Side::Buy, 100, 10);
    order.fill(4);

    EXPECT_EQ(order.getQuantity(), 6);
    EXPECT_FALSE(order.isFilled());
}

TEST(OrderTest, CompleteFill)
{
    Order order(1, Side::Buy, 100, 10);
    order.fill(10);

    EXPECT_EQ(order.getQuantity(), 0);
    EXPECT_TRUE(order.isFilled());
}

TEST(OrderTest, FillMoreThanQuantityThrows)
{
    Order order(1, Side::Buy, 100, 5);
    EXPECT_THROW(order.fill(10), OrderError);
}

TEST(OrderTest, FillZeroThrows)
{
    Order order(1, Side::Buy, 100, 5);
    EXPECT_THROW(order.fill(0), OrderError);
}

// ─── Side конвертация ─────────────────────────────────────────────────────────

TEST(OrderTest, SideFromStringBuy)
{
    EXPECT_EQ(Order::sideFromString("BUY"), Side::Buy);
}

TEST(OrderTest, SideFromStringSell)
{
    EXPECT_EQ(Order::sideFromString("SELL"), Side::Sell);
}

TEST(OrderTest, SideFromStringUnknownThrows)
{
    EXPECT_THROW(Order::sideFromString("HOLD"), OrderError);
}

TEST(OrderTest, SideToStringBuy)
{
    EXPECT_EQ(Order::sideToString(Side::Buy), "BUY");
}

TEST(OrderTest, SideToStringSell)
{
    EXPECT_EQ(Order::sideToString(Side::Sell), "SELL");
}