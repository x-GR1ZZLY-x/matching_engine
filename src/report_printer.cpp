#include<iostream>
#include"report_printer.hpp"
#include"order.hpp"

namespace matching_engine{

void ReportPrinter::printTrade(const Trade& trade) const{
    std::cout << "TRADE"
        << " buy=" << trade.getBuyOrderId()
        << " sell=" << trade.getSellOrderId()
        << " price=" << trade.getPrice()
        << " quantity=" << trade.getQuantity()
        << "\n";
    std::cout.flush();
}

void ReportPrinter::printOrderBook(const OrderBook& book) const{
    std::cout << "ORDER BOOK\n\n";

    std::cout << "SELL\n";
    const auto sells = book.sellOrders();
    for(const auto& order : sells) {
        std::cout << order->getPrice() << " " << order->getQuantity() << "\n";
    }
    std::cout << "\n";

    std::cout << "BUY\n";
    const auto buys = book.buyOrders();
    for(const auto& order : buys){
        std::cout << order->getPrice() << " " << order->getQuantity() << "\n";
    }

    std::cout << "\n";
    std::cout.flush();
}

void ReportPrinter::printError(const std::string& message) const{
    std::cerr << "ERROR: " << message << "\n";
    std::cout.flush();
}

void ReportPrinter::printReplaySummary(long long processedCommands, long long trades,
    long long duplicates, long long skippedLines, std::chrono::milliseconds elapsed) const{
    std::cout << "REPLAY SUMMARY\n"
        << "processed=" << processedCommands
        << " trades=" << trades
        << " duplicates=" << duplicates
        << " skipped=" << skippedLines
        << " elapsed_ms=" << elapsed.count()
        << "\n";
    std::cout.flush();
}

}