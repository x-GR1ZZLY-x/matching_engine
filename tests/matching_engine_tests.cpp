#include <gtest/gtest.h>
#include "matching_engine.hpp"

using namespace matching_engine;

// Вспомогательные фабрики команд
static AddCommand makeAdd(int id, Side side, int price, int qty)
{
    return AddCommand(id, side, price, qty);
}

static CancelCommand makeCancel(int id)
{
    return CancelCommand(id);
}

static MarketAddCommand marketBuy(int id, Side side, int qty){
    return MarketAddCommand(id, side, qty);
}
// ─── ADD без совпадений ───────────────────────────────────────────────────────

TEST(MatchingEngineTest, AddBuyNoMatch)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Buy, 100, 10));

    EXPECT_TRUE(engine.trades().empty());
    EXPECT_NE(engine.orderBook().findOrder(1), nullptr);
}

TEST(MatchingEngineTest, AddSellNoMatch)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Sell, 200, 5));

    EXPECT_TRUE(engine.trades().empty());
    EXPECT_NE(engine.orderBook().findOrder(1), nullptr);
}

TEST(MatchingEngineTest, BuyAndSellPricesDoNotCross)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Buy,  100, 10));
    engine.process(makeAdd(2, Side::Sell, 200,  5));

    EXPECT_TRUE(engine.trades().empty());
    EXPECT_NE(engine.orderBook().findOrder(1), nullptr);
    EXPECT_NE(engine.orderBook().findOrder(2), nullptr);
}

// ─── Полное исполнение ────────────────────────────────────────────────────────

TEST(MatchingEngineTest, FullMatchBuyIncoming)
{
    MatchingEngine engine;
    // SELL сначала встаёт в книгу
    engine.process(makeAdd(1, Side::Sell, 100, 10));
    // BUY приходит и полностью матчится
    engine.process(makeAdd(2, Side::Buy,  100, 10));

    ASSERT_EQ(engine.trades().size(), 1u);
    const auto& t = engine.trades()[0];
    EXPECT_EQ(t.getBuyOrderId(),  2);
    EXPECT_EQ(t.getSellOrderId(), 1);
    EXPECT_EQ(t.getPrice(),       100); // цена заявки из книги
    EXPECT_EQ(t.getQuantity(),    10);

    // Обе заявки полностью исполнены — их нет в книге
    EXPECT_EQ(engine.orderBook().findOrder(1), nullptr);
    EXPECT_EQ(engine.orderBook().findOrder(2), nullptr);
}

TEST(MatchingEngineTest, FullMatchSellIncoming)
{
    MatchingEngine engine;
    // BUY сначала встаёт в книгу
    engine.process(makeAdd(1, Side::Buy,  100, 10));
    // SELL приходит и полностью матчится
    engine.process(makeAdd(2, Side::Sell, 100, 10));

    ASSERT_EQ(engine.trades().size(), 1u);
    const auto& t = engine.trades()[0];
    EXPECT_EQ(t.getBuyOrderId(),  1);
    EXPECT_EQ(t.getSellOrderId(), 2);
    EXPECT_EQ(t.getPrice(),       100);
    EXPECT_EQ(t.getQuantity(),    10);
}

// ─── Частичное исполнение ─────────────────────────────────────────────────────

TEST(MatchingEngineTest, PartialMatchIncomingBuyLarger)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Sell, 100,  5));
    engine.process(makeAdd(2, Side::Buy,  100, 10));

    ASSERT_EQ(engine.trades().size(), 1u);
    EXPECT_EQ(engine.trades()[0].getQuantity(), 5);

    // SELL полностью исполнен — его нет в книге
    EXPECT_EQ(engine.orderBook().findOrder(1), nullptr);
    // BUY остаток 5 — он в книге
    auto rem = engine.orderBook().findOrder(2);
    ASSERT_NE(rem, nullptr);
    EXPECT_EQ(rem->getQuantity(), 5);
}

TEST(MatchingEngineTest, PartialMatchIncomingSellLarger)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Buy,  100,  5));
    engine.process(makeAdd(2, Side::Sell, 100, 10));

    ASSERT_EQ(engine.trades().size(), 1u);
    EXPECT_EQ(engine.trades()[0].getQuantity(), 5);

    EXPECT_EQ(engine.orderBook().findOrder(1), nullptr);
    auto rem = engine.orderBook().findOrder(2);
    ASSERT_NE(rem, nullptr);
    EXPECT_EQ(rem->getQuantity(), 5);
}

// ─── Цена сделки — по заявке из книги ────────────────────────────────────────

TEST(MatchingEngineTest, TradePriceIsBookOrderPrice)
{
    MatchingEngine engine;
    // SELL стоит в книге по 95, BUY приходит по 100 — цена сделки 95
    engine.process(makeAdd(1, Side::Sell,  95, 10));
    engine.process(makeAdd(2, Side::Buy,  100, 10));

    ASSERT_EQ(engine.trades().size(), 1u);
    EXPECT_EQ(engine.trades()[0].getPrice(), 95);
}

TEST(MatchingEngineTest, TradePriceIsBookBuyPrice)
{
    MatchingEngine engine;
    // BUY стоит в книге по 105, SELL приходит по 100 — цена сделки 105
    engine.process(makeAdd(1, Side::Buy,  105, 10));
    engine.process(makeAdd(2, Side::Sell, 100, 10));

    ASSERT_EQ(engine.trades().size(), 1u);
    EXPECT_EQ(engine.trades()[0].getPrice(), 105);
}

// ─── Несколько сделок подряд ──────────────────────────────────────────────────

TEST(MatchingEngineTest, MultipleTradesOneIncoming)
{
    MatchingEngine engine;
    // Два SELL в книге
    engine.process(makeAdd(1, Side::Sell, 100, 3));
    engine.process(makeAdd(2, Side::Sell, 100, 3));
    // BUY на 6 — матчится с обоими
    engine.process(makeAdd(3, Side::Buy,  100, 6));

    ASSERT_EQ(engine.trades().size(), 2u);
    EXPECT_EQ(engine.trades()[0].getSellOrderId(), 1);
    EXPECT_EQ(engine.trades()[1].getSellOrderId(), 2);

    // Все три заявки полностью исполнены
    EXPECT_EQ(engine.orderBook().findOrder(1), nullptr);
    EXPECT_EQ(engine.orderBook().findOrder(2), nullptr);
    EXPECT_EQ(engine.orderBook().findOrder(3), nullptr);
}

TEST(MatchingEngineTest, PricePriorityBestSellMatchedFirst)
{
    MatchingEngine engine;
    // Два SELL: 110 и 100. Лучшая цена для BUY — 100
    engine.process(makeAdd(1, Side::Sell, 110, 5));
    engine.process(makeAdd(2, Side::Sell, 100, 5));
    engine.process(makeAdd(3, Side::Buy,  110, 5));

    ASSERT_EQ(engine.trades().size(), 1u);
    // Должна исполниться заявка с ценой 100 (лучшая SELL)
    EXPECT_EQ(engine.trades()[0].getSellOrderId(), 2);
    EXPECT_EQ(engine.trades()[0].getPrice(),       100);
}

// ─── CANCEL ───────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, CancelExistingOrder)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Buy, 100, 10));
    engine.process(makeCancel(1));

    EXPECT_EQ(engine.orderBook().findOrder(1), nullptr);
    EXPECT_TRUE(engine.trades().empty());
}

TEST(MatchingEngineTest, CancelNonExistentThrows)
{
    MatchingEngine engine;
    EXPECT_THROW(engine.process(makeCancel(99)), OrderBookError);
}

TEST(MatchingEngineTest, CancelledOrderDoesNotMatch)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Sell, 100, 10));
    engine.process(makeCancel(1));
    engine.process(makeAdd(2, Side::Buy,  100, 10));

    EXPECT_TRUE(engine.trades().empty());
    // BUY встала в книгу, так как SELL уже отменён
    EXPECT_NE(engine.orderBook().findOrder(2), nullptr);
}

// ─── Дубликаты ────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, DuplicateOrderIdThrows)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Buy, 100, 10));
    EXPECT_THROW(engine.process(makeAdd(1, Side::Sell, 200, 5)), DuplicateOrderError);
}

// ─── Накопление сделок ────────────────────────────────────────────────────────

TEST(MatchingEngineTest, TradesAccumulate)
{
    MatchingEngine engine;
    engine.process(makeAdd(1, Side::Sell, 100, 5));
    engine.process(makeAdd(2, Side::Buy,  100, 5));
    engine.process(makeAdd(3, Side::Sell, 100, 5));
    engine.process(makeAdd(4, Side::Buy,  100, 5));

    EXPECT_EQ(engine.trades().size(), 2u);
}


TEST(MatchingEngineTest, MarketBuyFullyFilled) {
    MatchingEngine engine;

    AddCommand sellCmd(1, Side::Sell, 100, 5);
    engine.process(sellCmd);

    MarketAddCommand marketBuy(2, Side::Buy, 5);
    engine.process(marketBuy);

    const auto& trades = engine.trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].getPrice(),    100);
    EXPECT_EQ(trades[0].getQuantity(), 5);
    EXPECT_EQ(trades[0].getBuyOrderId(),  2);
    EXPECT_EQ(trades[0].getSellOrderId(), 1);

    EXPECT_EQ(engine.orderBook().bestSell(), nullptr);
}

TEST(MatchingEngineTest, MarketBuyPartiallyFilled) {
    MatchingEngine engine;

    AddCommand sellCmd(1, Side::Sell, 100, 3);
    engine.process(sellCmd);

    MarketAddCommand marketBuy(2, Side::Buy, 10);
    engine.process(marketBuy);

    const auto& trades = engine.trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].getQuantity(), 3);

    EXPECT_EQ(engine.orderBook().findOrder(2), nullptr);
}

TEST(MatchingEngineTest, MarketBuyNoSellers) {
    MatchingEngine engine;

    MarketAddCommand marketBuy(1, Side::Buy, 10);
    engine.process(marketBuy);

    EXPECT_TRUE(engine.trades().empty());
    EXPECT_EQ(engine.orderBook().findOrder(1), nullptr);
}

TEST(MatchingEngineTest, MarketSellFullyFilled) {
    MatchingEngine engine;

    AddCommand buyCmd(1, Side::Buy, 100, 5);
    engine.process(buyCmd);

    MarketAddCommand marketSell(2, Side::Sell, 5);
    engine.process(marketSell);

    const auto& trades = engine.trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].getPrice(),    100);
    EXPECT_EQ(trades[0].getQuantity(), 5);
    EXPECT_EQ(trades[0].getBuyOrderId(),  1);
    EXPECT_EQ(trades[0].getSellOrderId(), 2);
}

TEST(MatchingEngineTest, MarketBuyMatchesMultipleLevels) {
    MatchingEngine engine;

    AddCommand sell1(1, Side::Sell, 100, 3);
    AddCommand sell2(2, Side::Sell, 101, 4);
    engine.process(sell1);
    engine.process(sell2);

    MarketAddCommand marketBuy(3, Side::Buy, 7);
    engine.process(marketBuy);

    const auto& trades = engine.trades();
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].getPrice(),    100);
    EXPECT_EQ(trades[0].getQuantity(), 3);
    EXPECT_EQ(trades[1].getPrice(),    101);
    EXPECT_EQ(trades[1].getQuantity(), 4);

    EXPECT_EQ(engine.orderBook().bestSell(), nullptr);
}

TEST(MatchingEngineTest, MarketOrderNotLeftInBook) {
    MatchingEngine engine;

    MarketAddCommand marketBuy(1, Side::Buy, 5);
    engine.process(marketBuy);

    EXPECT_EQ(engine.orderBook().findOrder(1), nullptr);
    EXPECT_TRUE(engine.orderBook().empty());
}


TEST(MatchingEngineTest, ModifyChangesPrice) {
    MatchingEngine engine;

    AddCommand buyCmd(1, Side::Buy, 100, 10);
    engine.process(buyCmd);

    ModifyCommand modCmd(1, 110, 5);
    engine.process(modCmd);

    auto order = engine.orderBook().findOrder(1);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->getPrice(),    110);
    EXPECT_EQ(order->getQuantity(), 5);
}

TEST(MatchingEngineTest, ModifyTriggersMatch) {
    MatchingEngine engine;

    AddCommand sellCmd(1, Side::Sell, 105, 5);
    engine.process(sellCmd);

    AddCommand buyCmd(2, Side::Buy, 100, 10);
    engine.process(buyCmd);

    EXPECT_TRUE(engine.trades().empty());

    ModifyCommand modCmd(2, 105, 10);
    engine.process(modCmd);

    const auto& trades = engine.trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].getPrice(),    105);
    EXPECT_EQ(trades[0].getQuantity(), 5);
}

TEST(MatchingEngineTest, ModifyLosesTimePriority) {
    MatchingEngine engine;

    AddCommand buy1(1, Side::Buy, 100, 5);
    AddCommand buy2(2, Side::Buy, 100, 5);
    engine.process(buy1);
    engine.process(buy2);

    ModifyCommand modCmd(1, 100, 5);
    engine.process(modCmd);

    AddCommand sellCmd(3, Side::Sell, 100, 5);
    engine.process(sellCmd);

    const auto& trades = engine.trades();
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].getBuyOrderId(), 2);
}

TEST(MatchingEngineTest, ModifyNonExistentOrder) {
    MatchingEngine engine;

    ModifyCommand modCmd(999, 100, 10);
    EXPECT_THROW(engine.process(modCmd), OrderBookError);
}