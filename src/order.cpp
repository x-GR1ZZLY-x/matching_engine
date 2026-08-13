#include "order.hpp"

namespace matching_engine{

Order::Order(int id, Side side, int price, int quantity)
    : id_(id), side_(side), price_(price), quantity_(quantity){
    
    if(price <= 0){
        throw OrderError("Negative price");
    }
    if(quantity <= 0){
        throw OrderError("Negative quantity");
    }
}

void Order::fill(int quantity){
    if(quantity <= 0){
        throw OrderError("Negative quantity");
    }
    if(quantity > quantity_){
        throw OrderError("Quantity exceeded");
    }
    quantity_ -= quantity;
}

std::string Order::sideToString(Side side){
    return (side == Side::Buy) ? "BUY" : "SELL";
}

Side Order::sideFromString(const std::string& str){
    if(str == "BUY") return Side::Buy;
    if(str == "SELL") return Side::Sell;

    throw OrderError("Unknown order side");
}

MarketOrder::MarketOrder(int id, Side side, int quantity)
    : id_(id), side_(side), quantity_(quantity)
{
    if (quantity <= 0) {
        throw OrderError("Quantity must be positive");
    }
}

void MarketOrder::fill(int quantity) {
    if (quantity <= 0)        throw OrderError("Fill quantity must be positive");
    if (quantity > quantity_) throw OrderError("Fill quantity exceeded");
    quantity_ -= quantity;
}

}