#include<gtest/gtest.h>
#include"command_cache.hpp"

using namespace matching_engine;

namespace{

ExecutionResult sampleResult(int quantity){
    ExecutionResult result;
    result.trades.emplace_back(1, 2, 100, quantity);
    result.orderChanges.push_back(OrderChange{
        1, Side::Buy, std::optional<int>(100), 10, 10 - quantity,
        OrderStatus::PartiallyFilled, 1
    });
    return result;
}

}

TEST(CommandCacheTest, FindReturnsNullptrForUnknownId){
    CommandCache cache;
    EXPECT_EQ(cache.find("missing"), nullptr);
}

TEST(CommandCacheTest, PutThenFindReturnsStoredResult){
    CommandCache cache;
    cache.put("cmd-1", sampleResult(5));

    const ExecutionResult* found = cache.find("cmd-1");
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->trades.size(), 1u);
    EXPECT_EQ(found->trades[0].getQuantity(), 5);
    ASSERT_EQ(found->orderChanges.size(), 1u);
    EXPECT_EQ(found->orderChanges[0].remainingQuantity, 5);
}

TEST(CommandCacheTest, PutWithSameIdOverwritesPreviousResult){
    CommandCache cache;
    cache.put("cmd-1", sampleResult(5));
    cache.put("cmd-1", sampleResult(9));

    const ExecutionResult* found = cache.find("cmd-1");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->trades[0].getQuantity(), 9);
}

TEST(CommandCacheTest, DifferentIdsAreIndependent){
    CommandCache cache;
    cache.put("cmd-1", sampleResult(5));
    cache.put("cmd-2", sampleResult(9));

    const ExecutionResult* first = cache.find("cmd-1");
    const ExecutionResult* second = cache.find("cmd-2");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->trades[0].getQuantity(), 5);
    EXPECT_EQ(second->trades[0].getQuantity(), 9);
}

// Прогрев кеша — это просто последовательность put() с
// записями, восстановленными из processed_commands; здесь проверяем только
// то, что put() поддерживает многократную запись до первого find(), не
// требуя отдельного метода массовой загрузки.
TEST(CommandCacheTest, SupportsWarmUpByRepeatedPut){
    CommandCache cache;
    cache.put("cmd-1", sampleResult(1));
    cache.put("cmd-2", sampleResult(2));
    cache.put("cmd-3", sampleResult(3));

    EXPECT_EQ(cache.find("cmd-1")->trades[0].getQuantity(), 1);
    EXPECT_EQ(cache.find("cmd-2")->trades[0].getQuantity(), 2);
    EXPECT_EQ(cache.find("cmd-3")->trades[0].getQuantity(), 3);
}
