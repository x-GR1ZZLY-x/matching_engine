#include <gtest/gtest.h>
#include "order.hpp"
#include "sequence_generator.hpp"

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

// ─── Статус заявки ────────────────────────────────────────────────────────────

TEST(OrderTest, NewOrderIsOpen)
{
    SequenceGenerator::instance().reset(1);
    Order order(1, Side::Buy, 100, 10);

    EXPECT_EQ(order.getStatus(), OrderStatus::Open);
}

TEST(OrderTest, PartialFillSetsPartiallyFilledStatus)
{
    SequenceGenerator::instance().reset(1);
    Order order(1, Side::Buy, 100, 10);
    order.fill(4);

    EXPECT_EQ(order.getStatus(), OrderStatus::PartiallyFilled);
}

TEST(OrderTest, CompleteFillSetsFilledStatus)
{
    SequenceGenerator::instance().reset(1);
    Order order(1, Side::Buy, 100, 10);
    order.fill(10);

    EXPECT_EQ(order.getStatus(), OrderStatus::Filled);
}

TEST(OrderTest, CancelSetsCancelledStatus)
{
    SequenceGenerator::instance().reset(1);
    Order order(1, Side::Buy, 100, 10);
    order.cancel();

    EXPECT_EQ(order.getStatus(), OrderStatus::Cancelled);
}

TEST(OrderTest, InitialQuantityDoesNotChangeOnFill)
{
    SequenceGenerator::instance().reset(1);
    Order order(1, Side::Buy, 100, 10);
    order.fill(4);
    order.fill(3);

    EXPECT_EQ(order.getInitialQuantity(), 10);
    EXPECT_EQ(order.getQuantity(), 3);
}

TEST(OrderTest, StatusToStringMatchesSchemaConstraint)
{
    EXPECT_EQ(Order::statusToString(OrderStatus::Open), "OPEN");
    EXPECT_EQ(Order::statusToString(OrderStatus::PartiallyFilled), "PARTIALLY_FILLED");
    EXPECT_EQ(Order::statusToString(OrderStatus::Filled), "FILLED");
    EXPECT_EQ(Order::statusToString(OrderStatus::Cancelled), "CANCELLED");
}

TEST(OrderTest, StatusFromStringRoundTrips)
{
    EXPECT_EQ(Order::statusFromString("OPEN"), OrderStatus::Open);
    EXPECT_EQ(Order::statusFromString("PARTIALLY_FILLED"), OrderStatus::PartiallyFilled);
    EXPECT_EQ(Order::statusFromString("FILLED"), OrderStatus::Filled);
    EXPECT_EQ(Order::statusFromString("CANCELLED"), OrderStatus::Cancelled);
}

TEST(OrderTest, StatusFromStringUnknownThrows)
{
    EXPECT_THROW(Order::statusFromString("UNKNOWN"), OrderError);
}

// ─── Номер последовательности ─────────────────────────────────────────────────

TEST(OrderTest, ConsecutiveOrdersGetIncreasingSequenceNumbers)
{
    SequenceGenerator::instance().reset(1);

    Order first(1, Side::Buy, 100, 10);
    Order second(2, Side::Buy, 100, 10);

    EXPECT_LT(first.getSequenceNumber(), second.getSequenceNumber());
}

TEST(OrderTest, SequenceGeneratorResetIsPickedUpByNextOrder)
{
    SequenceGenerator::instance().reset(777);

    Order order(1, Side::Buy, 100, 10);

    EXPECT_EQ(order.getSequenceNumber(), 777);
}

// ─── Восстанавливающий конструктор ────────────────────────────────────────────

TEST(OrderTest, RestoringConstructorDoesNotAdvanceGenerator)
{
    SequenceGenerator::instance().reset(1);

    Order restored(1, Side::Buy, 100, 6, 10, 999, OrderStatus::PartiallyFilled);

    EXPECT_EQ(restored.getId(), 1);
    EXPECT_EQ(restored.getPrice(), 100);
    EXPECT_EQ(restored.getQuantity(), 6);
    EXPECT_EQ(restored.getInitialQuantity(), 10);
    EXPECT_EQ(restored.getSequenceNumber(), 999);
    EXPECT_EQ(restored.getStatus(), OrderStatus::PartiallyFilled);

    // Генератор счётчика не тронут восстанавливающим конструктором.
    EXPECT_EQ(SequenceGenerator::instance().next(), 1);
}

// ─── MarketOrder: статус, исходное количество, номер последовательности ──────

TEST(MarketOrderTest, NewOrderIsOpen)
{
    SequenceGenerator::instance().reset(1);
    MarketOrder order(1, Side::Buy, 10);

    EXPECT_EQ(order.getStatus(), OrderStatus::Open);
    EXPECT_EQ(order.getInitialQuantity(), 10);
}

TEST(MarketOrderTest, PartialFillSetsPartiallyFilledStatus)
{
    SequenceGenerator::instance().reset(1);
    MarketOrder order(1, Side::Buy, 10);
    order.fill(4);

    EXPECT_EQ(order.getStatus(), OrderStatus::PartiallyFilled);
    EXPECT_EQ(order.getInitialQuantity(), 10);
    EXPECT_EQ(order.getQuantity(), 6);
}

TEST(MarketOrderTest, CompleteFillSetsFilledStatus)
{
    SequenceGenerator::instance().reset(1);
    MarketOrder order(1, Side::Buy, 10);
    order.fill(10);

    EXPECT_EQ(order.getStatus(), OrderStatus::Filled);
}

TEST(MarketOrderTest, CancelSetsCancelledStatus)
{
    SequenceGenerator::instance().reset(1);
    MarketOrder order(1, Side::Buy, 10);
    order.cancel();

    EXPECT_EQ(order.getStatus(), OrderStatus::Cancelled);
}

TEST(MarketOrderTest, ConsecutiveOrdersGetIncreasingSequenceNumbers)
{
    SequenceGenerator::instance().reset(1);

    MarketOrder first(1, Side::Buy, 10);
    MarketOrder second(2, Side::Buy, 10);

    EXPECT_LT(first.getSequenceNumber(), second.getSequenceNumber());
}

TEST(MarketOrderTest, RestoringConstructorDoesNotAdvanceGenerator)
{
    SequenceGenerator::instance().reset(1);

    MarketOrder restored(1, Side::Buy, 6, 10, 999, OrderStatus::PartiallyFilled);

    EXPECT_EQ(restored.getId(), 1);
    EXPECT_EQ(restored.getQuantity(), 6);
    EXPECT_EQ(restored.getInitialQuantity(), 10);
    EXPECT_EQ(restored.getSequenceNumber(), 999);
    EXPECT_EQ(restored.getStatus(), OrderStatus::PartiallyFilled);

    EXPECT_EQ(SequenceGenerator::instance().next(), 1);
}

// ─── Отменённая заявка не исполняется ─────────────────────────────────────────

TEST(OrderTest, FillAfterCancelThrows)
{
    SequenceGenerator::instance().reset(1);
    Order order(1, Side::Buy, 100, 10);
    order.cancel();

    EXPECT_THROW(order.fill(4), OrderError);
    EXPECT_EQ(order.getStatus(), OrderStatus::Cancelled);
}

TEST(MarketOrderTest, FillAfterCancelThrows)
{
    SequenceGenerator::instance().reset(1);
    MarketOrder order(1, Side::Buy, 10);
    order.cancel();

    EXPECT_THROW(order.fill(4), OrderError);
    EXPECT_EQ(order.getStatus(), OrderStatus::Cancelled);
}

// ─── Полностью исполненная заявка не отменяется ───────────────────────────────

TEST(OrderTest, CancelAfterFullFillThrows)
{
    SequenceGenerator::instance().reset(1);
    Order order(1, Side::Buy, 100, 10);
    order.fill(10);

    EXPECT_THROW(order.cancel(), OrderError);
    EXPECT_EQ(order.getStatus(), OrderStatus::Filled);
}

TEST(MarketOrderTest, CancelAfterFullFillThrows)
{
    SequenceGenerator::instance().reset(1);
    MarketOrder order(1, Side::Buy, 10);
    order.fill(10);

    EXPECT_THROW(order.cancel(), OrderError);
    EXPECT_EQ(order.getStatus(), OrderStatus::Filled);
}

// ─── Восстанавливающий конструктор отвергает рассогласованные пары ───────────

TEST(OrderTest, RestoringConstructorRejectsZeroRemainingWithOpenStatus)
{
    EXPECT_THROW(Order(1, Side::Buy, 100, 0, 10, 5, OrderStatus::Open), OrderError);
}

TEST(OrderTest, RestoringConstructorRejectsNonzeroRemainingWithFilledStatus)
{
    EXPECT_THROW(Order(1, Side::Buy, 100, 4, 10, 5, OrderStatus::Filled), OrderError);
}

TEST(MarketOrderTest, RestoringConstructorRejectsZeroRemainingWithOpenStatus)
{
    EXPECT_THROW(MarketOrder(1, Side::Buy, 0, 10, 5, OrderStatus::Open), OrderError);
}

TEST(MarketOrderTest, RestoringConstructorRejectsNonzeroRemainingWithFilledStatus)
{
    EXPECT_THROW(MarketOrder(1, Side::Buy, 4, 10, 5, OrderStatus::Filled), OrderError);
}

TEST(OrderTest, RestoringConstructorRejectsOpenStatusWithPartialRemaining)
{
    EXPECT_THROW(Order(1, Side::Buy, 100, 6, 10, 5, OrderStatus::Open), OrderError);
}

TEST(OrderTest, RestoringConstructorRejectsPartiallyFilledStatusWithFullRemaining)
{
    EXPECT_THROW(Order(1, Side::Buy, 100, 10, 10, 5, OrderStatus::PartiallyFilled), OrderError);
}

TEST(MarketOrderTest, RestoringConstructorRejectsOpenStatusWithPartialRemaining)
{
    EXPECT_THROW(MarketOrder(1, Side::Buy, 6, 10, 5, OrderStatus::Open), OrderError);
}

TEST(MarketOrderTest, RestoringConstructorRejectsPartiallyFilledStatusWithFullRemaining)
{
    EXPECT_THROW(MarketOrder(1, Side::Buy, 10, 10, 5, OrderStatus::PartiallyFilled), OrderError);
}

TEST(OrderTest, RestoringConstructorRejectsNonPositiveSequenceNumber)
{
    EXPECT_THROW(Order(1, Side::Buy, 100, 6, 10, 0, OrderStatus::PartiallyFilled), OrderError);
    EXPECT_THROW(Order(1, Side::Buy, 100, 6, 10, -1, OrderStatus::PartiallyFilled), OrderError);
}

TEST(MarketOrderTest, RestoringConstructorRejectsNonPositiveSequenceNumber)
{
    EXPECT_THROW(MarketOrder(1, Side::Buy, 6, 10, 0, OrderStatus::PartiallyFilled), OrderError);
    EXPECT_THROW(MarketOrder(1, Side::Buy, 6, 10, -1, OrderStatus::PartiallyFilled), OrderError);
}

// ─── Восстанавливающий конструктор принимает законные состояния ──────────────

TEST(OrderTest, RestoringConstructorAcceptsCancelledPartiallyFilledOrder)
{
    Order restored(1, Side::Buy, 100, 6, 10, 5, OrderStatus::Cancelled);

    EXPECT_EQ(restored.getQuantity(), 6);
    EXPECT_EQ(restored.getInitialQuantity(), 10);
    EXPECT_EQ(restored.getStatus(), OrderStatus::Cancelled);
}

TEST(OrderTest, RestoringConstructorAcceptsPartiallyFilledOrder)
{
    Order restored(1, Side::Buy, 100, 6, 10, 5, OrderStatus::PartiallyFilled);

    EXPECT_EQ(restored.getQuantity(), 6);
    EXPECT_EQ(restored.getInitialQuantity(), 10);
    EXPECT_EQ(restored.getStatus(), OrderStatus::PartiallyFilled);
}

// ─── Отклонённая заявка не расходует номер последовательности ────────────────

TEST(OrderTest, RejectedOrderDoesNotAdvanceSequence)
{
    SequenceGenerator::instance().reset(100);

    EXPECT_THROW(Order(1, Side::Buy, 0, 10), OrderError);

    Order order(2, Side::Buy, 100, 10);
    EXPECT_EQ(order.getSequenceNumber(), 100);
}

TEST(MarketOrderTest, RejectedOrderDoesNotAdvanceSequence)
{
    SequenceGenerator::instance().reset(100);

    EXPECT_THROW(MarketOrder(1, Side::Buy, 0), OrderError);

    MarketOrder order(2, Side::Buy, 10);
    EXPECT_EQ(order.getSequenceNumber(), 100);
}