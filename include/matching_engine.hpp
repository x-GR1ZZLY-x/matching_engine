#pragma once

#include<memory>
#include<vector>
#include"command.hpp"
#include"order_book.hpp"
#include"trade.hpp"
#include"execution_result.hpp"

namespace matching_engine{

class MatchingEngine{
public:
    ExecutionResult process(const Command& command);
    const std::vector<Trade>& trades() const noexcept;
    const OrderBook& orderBook() const noexcept;

    // Восстановление (REQ-REC-04): помещает уже готовую заявку прямо в книгу,
    // минуя match/matchMarket. Тонкая обёртка над OrderBook::restore — сама
    // не содержит логики сопоставления.
    void restore(std::shared_ptr<Order> order);
private:
    OrderBook orderBook_;
    std::vector<Trade> trades_;

    ExecutionResult processAdd(const AddCommand& cmd);
    ExecutionResult processCancel(const CancelCommand& cmd);
    ExecutionResult match(std::shared_ptr<Order> incoming);
    std::shared_ptr<Trade> tryMatchBuy(std::shared_ptr<Order> buyOrder,
        std::vector<OrderChange>& changes);
    std::shared_ptr<Trade> tryMatchSell(std::shared_ptr<Order> sellOrder,
        std::vector<OrderChange>& changes);
    Trade executeTrade(std::shared_ptr<Order> bookOrder, std::shared_ptr<Order> incomingOrder,
        std::vector<OrderChange>& changes);

    ExecutionResult processMarketAdd(const MarketAddCommand& cmd);
    ExecutionResult processModify(const ModifyCommand& cmd);
    ExecutionResult matchMarket(std::shared_ptr<MarketOrder> incoming);
    std::shared_ptr<Trade> tryMatchBuyMarket(std::shared_ptr<MarketOrder> buyOrder,
        std::vector<OrderChange>& changes);
    std::shared_ptr<Trade> tryMatchSellMarket(std::shared_ptr<MarketOrder> sellOrder,
        std::vector<OrderChange>& changes);
    Trade executeMarketTrade(std::shared_ptr<Order> bookOrder,
        std::shared_ptr<MarketOrder> incomingOrder, std::vector<OrderChange>& changes);

    static OrderChange snapshot(const Order& order);
    static OrderChange snapshot(const MarketOrder& order);
};

}
