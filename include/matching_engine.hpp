#pragma once

#include<memory>
#include<vector>
#include"command.hpp"
#include"order_book.hpp"
#include"trade.hpp"

namespace matching_engine{

class MatchingEngine{
public:
    void process(const Command& command);
    const std::vector<Trade>& trades() const noexcept;
    const OrderBook& orderBook() const noexcept;
private:
    OrderBook orderBook_;
    std::vector<Trade> trades_;

    void processAdd(const AddCommand& cmd);
    void processCancel(const CancelCommand& cmd);
    std::vector<Trade> match(std::shared_ptr<Order> incoming);
    std::shared_ptr<Trade> tryMatchBuy(std::shared_ptr<Order> buyOrder);
    std::shared_ptr<Trade> tryMatchSell(std::shared_ptr<Order> sellOrder);
    Trade executeTrade(std::shared_ptr<Order> bookOrder, std::shared_ptr<Order> incomingOrder);

    void processMarketAdd(const MarketAddCommand& cmd);
    void processModify(const ModifyCommand& cmd);
    std::vector<Trade> matchMarket(std::shared_ptr<MarketOrder> incoming);
    std::shared_ptr<Trade> tryMatchBuyMarket(std::shared_ptr<MarketOrder> buyOrder);
    std::shared_ptr<Trade> tryMatchSellMarket(std::shared_ptr<MarketOrder> sellOrder);
    Trade executeMarketTrade(std::shared_ptr<Order> bookOrder, std::shared_ptr<MarketOrder> incomingOrder);
};

}