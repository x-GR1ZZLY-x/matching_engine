#include "order.hpp"
#include "sequence_generator.hpp"

namespace matching_engine{

Order::Order(int id, Side side, int price, int quantity)
    : id_(id), side_(side), price_(price), quantity_(quantity),
      initialQuantity_(quantity),
      sequenceNumber_(0),
      status_(OrderStatus::Open){

    if(price <= 0){
        throw OrderError("Negative price");
    }
    if(quantity <= 0){
        throw OrderError("Negative quantity");
    }

    sequenceNumber_ = SequenceGenerator::instance().next();
}

Order::Order(int id, Side side, int price, int quantity,
             int initialQuantity, long long sequenceNumber, OrderStatus status)
    : id_(id), side_(side), price_(price), quantity_(quantity),
      initialQuantity_(initialQuantity),
      sequenceNumber_(sequenceNumber),
      status_(status){

    if(price <= 0){
        throw OrderError("Negative price");
    }
    if(initialQuantity <= 0){
        throw OrderError("Negative quantity");
    }
    if(quantity < 0 || quantity > initialQuantity){
        throw OrderError("Remaining quantity out of range");
    }
    if(sequenceNumber <= 0){
        throw OrderError("Sequence number must be positive");
    }
    if(quantity == 0){
        if(status != OrderStatus::Filled && status != OrderStatus::Cancelled){
            throw OrderError("Status inconsistent with zero remaining quantity");
        }
    } else {
        if(status == OrderStatus::Filled){
            throw OrderError("Status inconsistent with nonzero remaining quantity");
        }
        if(status == OrderStatus::Open && quantity < initialQuantity){
            throw OrderError("Status inconsistent with partially filled remaining quantity");
        }
        if(status == OrderStatus::PartiallyFilled && quantity == initialQuantity){
            throw OrderError("Status inconsistent with untouched remaining quantity");
        }
    }
}

void Order::fill(int quantity){
    if(status_ == OrderStatus::Cancelled){
        throw OrderError("Cannot fill a cancelled order");
    }
    if(quantity <= 0){
        throw OrderError("Negative quantity");
    }
    if(quantity > quantity_){
        throw OrderError("Quantity exceeded");
    }
    quantity_ -= quantity;
    status_ = (quantity_ == 0) ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
}

void Order::cancel(){
    if(status_ == OrderStatus::Filled){
        throw OrderError("Cannot cancel a filled order");
    }
    status_ = OrderStatus::Cancelled;
}

std::string Order::sideToString(Side side){
    return (side == Side::Buy) ? "BUY" : "SELL";
}

Side Order::sideFromString(const std::string& str){
    if(str == "BUY") return Side::Buy;
    if(str == "SELL") return Side::Sell;

    throw OrderError("Unknown order side");
}

std::string Order::statusToString(OrderStatus status){
    switch(status){
        case OrderStatus::Open:            return "OPEN";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Cancelled:       return "CANCELLED";
    }
    throw OrderError("Unknown order status");
}

OrderStatus Order::statusFromString(const std::string& str){
    if(str == "OPEN") return OrderStatus::Open;
    if(str == "PARTIALLY_FILLED") return OrderStatus::PartiallyFilled;
    if(str == "FILLED") return OrderStatus::Filled;
    if(str == "CANCELLED") return OrderStatus::Cancelled;

    throw OrderError("Unknown order status");
}

MarketOrder::MarketOrder(int id, Side side, int quantity)
    : id_(id), side_(side), quantity_(quantity),
      initialQuantity_(quantity),
      sequenceNumber_(0),
      status_(OrderStatus::Open)
{
    if (quantity <= 0) {
        throw OrderError("Quantity must be positive");
    }

    sequenceNumber_ = SequenceGenerator::instance().next();
}

MarketOrder::MarketOrder(int id, Side side, int quantity,
                          int initialQuantity, long long sequenceNumber, OrderStatus status)
    : id_(id), side_(side), quantity_(quantity),
      initialQuantity_(initialQuantity),
      sequenceNumber_(sequenceNumber),
      status_(status)
{
    if (initialQuantity <= 0) {
        throw OrderError("Quantity must be positive");
    }
    if (quantity < 0 || quantity > initialQuantity) {
        throw OrderError("Remaining quantity out of range");
    }
    if (sequenceNumber <= 0) {
        throw OrderError("Sequence number must be positive");
    }
    if (quantity == 0) {
        if (status != OrderStatus::Filled && status != OrderStatus::Cancelled) {
            throw OrderError("Status inconsistent with zero remaining quantity");
        }
    } else {
        if (status == OrderStatus::Filled) {
            throw OrderError("Status inconsistent with nonzero remaining quantity");
        }
        if (status == OrderStatus::Open && quantity < initialQuantity) {
            throw OrderError("Status inconsistent with partially filled remaining quantity");
        }
        if (status == OrderStatus::PartiallyFilled && quantity == initialQuantity) {
            throw OrderError("Status inconsistent with untouched remaining quantity");
        }
    }
}

void MarketOrder::fill(int quantity) {
    if (status_ == OrderStatus::Cancelled) throw OrderError("Cannot fill a cancelled order");
    if (quantity <= 0)        throw OrderError("Fill quantity must be positive");
    if (quantity > quantity_) throw OrderError("Fill quantity exceeded");
    quantity_ -= quantity;
    status_ = (quantity_ == 0) ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
}

void MarketOrder::cancel(){
    if (status_ == OrderStatus::Filled) throw OrderError("Cannot cancel a filled order");
    status_ = OrderStatus::Cancelled;
}

}
