#pragma once
#include<vector>
#include"trade.hpp"
#include"order_book.hpp"

namespace matching_engine{

class ReportPrinter{
public:
    void printTrade(const Trade& trade) const;
    void printOrderBook(const OrderBook& book) const;
    void printError(const std::string& message) const;
};

}