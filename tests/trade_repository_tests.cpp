#include<gtest/gtest.h>
#include<optional>
#include<string>
#include<vector>
#include"execution_result.hpp"
#include"order.hpp"
#include"order_repository.hpp"
#include"pg_connection.hpp"
#include"pg_transaction.hpp"
#include"test_database.hpp"
#include"trade.hpp"
#include"trade_repository.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

// Идентификаторы заявок и номера последовательности в тесте — из того же
// заведомо свободного диапазона, что и в order_repository_tests.cpp
// (9-значные номера с префиксом 900000...), чтобы не столкнуться ни с
// другими тестами, ни с будущими реальными данными (SequenceGenerator
// начинает выдачу с единицы).
//
// trades.buy_order_id/sell_order_id — внешние ключи на orders(order_id),
// поэтому заявки нужно сначала сохранить через OrderRepository, иначе
// вставка сделки будет отклонена ограничением внешнего ключа.
TEST(TradeRepositoryTest, LoadAllReturnsTradesOrderedByTradeId){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository orderRepo;
    orderRepo.save(conn, OrderChange{900000801, Side::Buy, 100, 10, 10,
        OrderStatus::Open, 900000801});
    orderRepo.save(conn, OrderChange{900000802, Side::Sell, 100, 10, 10,
        OrderStatus::Open, 900000802});

    TradeRepository tradeRepo;
    // trade_id выдаёт BIGSERIAL — приложение его не передаёт, поэтому
    // порядок вставки и есть ожидаемый порядок чтения.
    tradeRepo.insert(conn, Trade(900000801, 900000802, 100, 5));
    tradeRepo.insert(conn, Trade(900000801, 900000802, 100, 3));

    auto trades = tradeRepo.loadAll(conn);

    // Отбираем только тестовые сделки: относительный порядок среди них
    // сохраняется независимо от того, что ещё лежит в таблице.
    std::vector<Trade> testTrades;
    for(const auto& trade : trades){
        if(trade.getBuyOrderId() == 900000801 && trade.getSellOrderId() == 900000802){
            testTrades.push_back(trade);
        }
    }

    ASSERT_EQ(testTrades.size(), 2u);
    EXPECT_EQ(testTrades[0].getPrice(), 100);
    EXPECT_EQ(testTrades[0].getQuantity(), 5);
    EXPECT_EQ(testTrades[1].getPrice(), 100);
    EXPECT_EQ(testTrades[1].getQuantity(), 3);
}
