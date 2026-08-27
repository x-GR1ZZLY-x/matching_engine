#include <gtest/gtest.h>
#include "order_book.hpp"

using namespace matching_engine;

// Вспомогательная фабрика
static std::shared_ptr<Order> makeBuy(int id, int price, int qty)
{
    return std::make_shared<Order>(id, Side::Buy, price, qty);
}

static std::shared_ptr<Order> makeSell(int id, int price, int qty)
{
    return std::make_shared<Order>(id, Side::Sell, price, qty);
}

// ─── Добавление заявок ────────────────────────────────────────────────────────

TEST(OrderBookTest, AddAndFindBuyOrder)
{
    OrderBook book;
    book.addOrder(makeBuy(1, 100, 10));
    auto found = book.findOrder(1);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->getId(),       1);
    EXPECT_EQ(found->getPrice(),    100);
    EXPECT_EQ(found->getQuantity(), 10);
}

TEST(OrderBookTest, AddAndFindSellOrder)
{
    OrderBook book;
    book.addOrder(makeSell(2, 200, 5));
    auto found = book.findOrder(2);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->getSide(), Side::Sell);
}

TEST(OrderBookTest, FindNonExistentReturnsNullptr)
{
    OrderBook book;
    EXPECT_EQ(book.findOrder(999), nullptr);
}

TEST(OrderBookTest, DuplicateIdThrows)
{
    OrderBook book;
    book.addOrder(makeBuy(1, 100, 10));
    EXPECT_THROW(book.addOrder(makeBuy(1, 200, 5)), DuplicateOrderError);
}

// ─── Удаление заявок ─────────────────────────────────────────────────────────

TEST(OrderBookTest, RemoveExistingOrder)
{
    OrderBook book;
    book.addOrder(makeBuy(1, 100, 10));
    book.removeOrder(1);
    EXPECT_EQ(book.findOrder(1), nullptr);
}

TEST(OrderBookTest, RemoveNonExistentThrows)
{
    OrderBook book;
    EXPECT_THROW(book.removeOrder(42), OrderBookError);
}

TEST(OrderBookTest, EmptyAfterRemovingAllOrders)
{
    OrderBook book;
    book.addOrder(makeBuy(1, 100, 10));
    book.addOrder(makeSell(2, 200, 5));
    book.removeOrder(1);
    book.removeOrder(2);
    EXPECT_TRUE(book.empty());
}

// ─── bestBuy / bestSell ───────────────────────────────────────────────────────

TEST(OrderBookTest, BestBuyIsMaxPrice)
{
    OrderBook book;
    book.addOrder(makeBuy(1, 100, 10));
    book.addOrder(makeBuy(2, 150, 5));
    book.addOrder(makeBuy(3, 120, 7));

    auto best = book.bestBuy();
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->getPrice(), 150);
    EXPECT_EQ(best->getId(),    2);
}

TEST(OrderBookTest, BestSellIsMinPrice)
{
    OrderBook book;
    book.addOrder(makeSell(1, 200, 10));
    book.addOrder(makeSell(2, 150, 5));
    book.addOrder(makeSell(3, 180, 7));

    auto best = book.bestSell();
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->getPrice(), 150);
    EXPECT_EQ(best->getId(),    2);
}

TEST(OrderBookTest, BestBuyNullptrWhenEmpty)
{
    OrderBook book;
    EXPECT_EQ(book.bestBuy(), nullptr);
}

TEST(OrderBookTest, BestSellNullptrWhenEmpty)
{
    OrderBook book;
    EXPECT_EQ(book.bestSell(), nullptr);
}

TEST(OrderBookTest, BestBuyUpdatesAfterRemoval)
{
    OrderBook book;
    book.addOrder(makeBuy(1, 150, 10));
    book.addOrder(makeBuy(2, 100, 5));
    book.removeOrder(1);

    auto best = book.bestBuy();
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->getPrice(), 100);
}

// ─── Price-Time Priority ─────────────────────────────────────────────────────

TEST(OrderBookTest, BuyOrdersDescendingPrice)
{
    OrderBook book;
    book.addOrder(makeBuy(1, 100, 1));
    book.addOrder(makeBuy(2, 150, 1));
    book.addOrder(makeBuy(3, 120, 1));

    auto orders = book.buyOrders();
    ASSERT_EQ(orders.size(), 3u);
    EXPECT_EQ(orders[0]->getPrice(), 150);
    EXPECT_EQ(orders[1]->getPrice(), 120);
    EXPECT_EQ(orders[2]->getPrice(), 100);
}

TEST(OrderBookTest, SellOrdersAscendingPrice)
{
    OrderBook book;
    book.addOrder(makeSell(1, 200, 1));
    book.addOrder(makeSell(2, 150, 1));
    book.addOrder(makeSell(3, 180, 1));

    auto orders = book.sellOrders();
    ASSERT_EQ(orders.size(), 3u);
    EXPECT_EQ(orders[0]->getPrice(), 150);
    EXPECT_EQ(orders[1]->getPrice(), 180);
    EXPECT_EQ(orders[2]->getPrice(), 200);
}

TEST(OrderBookTest, TimePriorityWithinSamePrice)
{
    OrderBook book;
    // Три BUY по одной цене — порядок FIFO
    book.addOrder(makeBuy(10, 100, 5));
    book.addOrder(makeBuy(20, 100, 3));
    book.addOrder(makeBuy(30, 100, 7));

    auto orders = book.buyOrders();
    ASSERT_EQ(orders.size(), 3u);
    EXPECT_EQ(orders[0]->getId(), 10);
    EXPECT_EQ(orders[1]->getId(), 20);
    EXPECT_EQ(orders[2]->getId(), 30);
}

TEST(OrderBookTest, BestBuyTimePriority)
{
    OrderBook book;
    // Две заявки по одной цене — bestBuy должна вернуть первую (FIFO)
    book.addOrder(makeBuy(1, 100, 5));
    book.addOrder(makeBuy(2, 100, 3));

    auto best = book.bestBuy();
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->getId(), 1);
}

// ─── Восстановление (REQ-REC-04) ──────────────────────────────────────────────

// Заявка, поднятая через restore(), собрана восстанавливающим конструктором:
// у неё уже есть исходное количество, номер в последовательности и статус.
static std::shared_ptr<Order> restoredOrder(int id, Side side, int price, int qty,
    long long sequenceNumber)
{
    return std::make_shared<Order>(id, side, price, qty, qty, sequenceNumber,
        OrderStatus::Open);
}

TEST(OrderBookTest, RestoreAddsToBothIndexes)
{
    OrderBook book;
    book.restore(restoredOrder(1, Side::Buy, 100, 10, 1));

    auto found = book.findOrder(1);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->getPrice(), 100);
    EXPECT_EQ(book.bestBuy(), found);
}

TEST(OrderBookTest, RestoreDoesNotMatchCrossingOrders)
{
    OrderBook book;
    // SELL по 100 восстановлена первой
    book.restore(restoredOrder(1, Side::Sell, 100, 10, 1));
    // BUY по 150 пересекается по цене с уже стоящей SELL
    book.restore(restoredOrder(2, Side::Buy, 150, 5, 2));

    // Никакого сопоставления не произошло — обе заявки целы и в книге
    auto sell = book.findOrder(1);
    auto buy  = book.findOrder(2);
    ASSERT_NE(sell, nullptr);
    ASSERT_NE(buy, nullptr);
    EXPECT_EQ(sell->getQuantity(), 10);
    EXPECT_EQ(buy->getQuantity(), 5);
}

TEST(OrderBookTest, RestorePreservesOrderOfSequenceNumbers)
{
    OrderBook book;
    // Три заявки одной цены, восстановленные в порядке возрастания номера
    book.restore(restoredOrder(10, Side::Buy, 100, 5, 1));
    book.restore(restoredOrder(20, Side::Buy, 100, 3, 2));
    book.restore(restoredOrder(30, Side::Buy, 100, 7, 3));

    auto orders = book.buyOrders();
    ASSERT_EQ(orders.size(), 3u);
    EXPECT_EQ(orders[0]->getId(), 10);
    EXPECT_EQ(orders[1]->getId(), 20);
    EXPECT_EQ(orders[2]->getId(), 30);

    // bestBuy — первая восстановленная заявка (FIFO)
    auto best = book.bestBuy();
    ASSERT_NE(best, nullptr);
    EXPECT_EQ(best->getId(), 10);
}