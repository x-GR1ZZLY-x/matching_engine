#include<gtest/gtest.h>
#include<memory>
#include<optional>
#include"command_processor.hpp"

using namespace matching_engine;

namespace{

OrderChange makeChange(int id, Side side, std::optional<int> price, int initialQuantity,
    int remainingQuantity, OrderStatus status, long long sequenceNumber){
    return OrderChange{id, side, price, initialQuantity, remainingQuantity,
        status, sequenceNumber};
}

}

// Сериализация в processed_commands.result и
// разбор обратно живут рядом в CommandProcessor и обязаны быть точной парой —
// это тот самый round-trip тест, который ловит расхождение форматов, не
// требуя базы.
TEST(CommandProcessorTest, SerializeParseRoundTripPreservesTradesAndOrderChanges){
    ExecutionResult result;
    result.trades.push_back(Trade(1, 2, 100, 5));
    result.trades.push_back(Trade(3, 4, 150, 10));
    result.orderChanges.push_back(
        makeChange(1, Side::Buy, 100, 10, 5, OrderStatus::PartiallyFilled, 7));
    result.orderChanges.push_back(
        makeChange(2, Side::Sell, std::nullopt, 20, 0, OrderStatus::Cancelled, 8));

    ExecutionResult parsed =
        CommandProcessor::parseResult(CommandProcessor::serializeResult(result));

    ASSERT_EQ(parsed.trades.size(), 2u);
    EXPECT_EQ(parsed.trades[0].getBuyOrderId(), 1);
    EXPECT_EQ(parsed.trades[0].getSellOrderId(), 2);
    EXPECT_EQ(parsed.trades[0].getPrice(), 100);
    EXPECT_EQ(parsed.trades[0].getQuantity(), 5);
    EXPECT_EQ(parsed.trades[1].getBuyOrderId(), 3);
    EXPECT_EQ(parsed.trades[1].getSellOrderId(), 4);
    EXPECT_EQ(parsed.trades[1].getPrice(), 150);
    EXPECT_EQ(parsed.trades[1].getQuantity(), 10);

    ASSERT_EQ(parsed.orderChanges.size(), 2u);
    EXPECT_EQ(parsed.orderChanges[0].id, 1);
    EXPECT_EQ(parsed.orderChanges[0].side, Side::Buy);
    ASSERT_TRUE(parsed.orderChanges[0].price.has_value());
    EXPECT_EQ(*parsed.orderChanges[0].price, 100);
    EXPECT_EQ(parsed.orderChanges[0].initialQuantity, 10);
    EXPECT_EQ(parsed.orderChanges[0].remainingQuantity, 5);
    EXPECT_EQ(parsed.orderChanges[0].status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(parsed.orderChanges[0].sequenceNumber, 7);

    EXPECT_EQ(parsed.orderChanges[1].id, 2);
    EXPECT_EQ(parsed.orderChanges[1].side, Side::Sell);
    // MARKET-заявка сериализуется с price == nullopt (SQL NULL) — критично
    // отличать это от отсутствия поля или числа 0.
    EXPECT_FALSE(parsed.orderChanges[1].price.has_value());
    EXPECT_EQ(parsed.orderChanges[1].status, OrderStatus::Cancelled);
}

TEST(CommandProcessorTest, SerializeParseRoundTripHandlesEmptyResult){
    ExecutionResult result;

    ExecutionResult parsed =
        CommandProcessor::parseResult(CommandProcessor::serializeResult(result));

    EXPECT_TRUE(parsed.trades.empty());
    EXPECT_TRUE(parsed.orderChanges.empty());
}

// MODIFY кладёт в orderChanges два снимка одной заявки — старый в начале,
// новый в конце: порядок значим, побеждает
// последняя запись с этим id. Разбор обязан сохранить порядок массива.
TEST(CommandProcessorTest, SerializeParseRoundTripPreservesOrderOfDuplicateIds){
    ExecutionResult result;
    result.orderChanges.push_back(makeChange(5, Side::Buy, 100, 10, 10, OrderStatus::Open, 1));
    result.orderChanges.push_back(makeChange(5, Side::Buy, 110, 8, 8, OrderStatus::Open, 2));

    ExecutionResult parsed =
        CommandProcessor::parseResult(CommandProcessor::serializeResult(result));

    ASSERT_EQ(parsed.orderChanges.size(), 2u);
    EXPECT_EQ(parsed.orderChanges[0].sequenceNumber, 1);
    EXPECT_EQ(parsed.orderChanges[1].sequenceNumber, 2);
}

// restoreOrder кладёт заявки в книгу через
// MatchingEngine::restore -> OrderBook::restore, минуя сопоставление — даже
// пересекающиеся по цене BUY/SELL не должны породить сделку.
TEST(CommandProcessorTest, RestoreOrderPutsCrossingOrdersIntoBookWithoutMatching){
    CommandProcessor processor;

    auto buy = std::make_shared<Order>(1, Side::Buy, 100, 10, 10, 1, OrderStatus::Open);
    auto sell = std::make_shared<Order>(2, Side::Sell, 90, 5, 5, 2, OrderStatus::Open);

    processor.restoreOrder(buy);
    processor.restoreOrder(sell);

    // findOrder(...) != nullptr одной наличия в книге недостаточно: частично
    // исполненная заявка тоже осталась бы в книге. Проверяем, что остатки не
    // уменьшились — иначе сопоставление всё же произошло.
    ASSERT_NE(processor.orderBook().findOrder(1), nullptr);
    ASSERT_NE(processor.orderBook().findOrder(2), nullptr);
    EXPECT_EQ(processor.orderBook().findOrder(1)->getQuantity(), 10);
    EXPECT_EQ(processor.orderBook().findOrder(2)->getQuantity(), 5);
}

// Колонка processed_commands.result
// nullable — прогрев кеша обязан пережить NULL, а не упасть при старте.
TEST(CommandProcessorTest, WarmCacheAcceptsNullResultWithoutThrowing){
    CommandProcessor processor;

    EXPECT_NO_THROW(processor.warmCache("cmd-1", std::nullopt));
}
