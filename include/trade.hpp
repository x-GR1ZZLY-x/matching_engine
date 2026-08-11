#pragma once

namespace matching_engine{

class Trade{
public:
    Trade(int buyOrderId, int sellOrderId, int price, int quantity);

    int getBuyOrderId() const noexcept{return buyOrderId_; }
    int getSellOrderId() const noexcept {return sellOrderId_; }
    int getPrice() const noexcept {return price_; }
    int getQuantity() const noexcept {return quantity_; }

private:
    int buyOrderId_;
    int sellOrderId_;
    int price_;
    int quantity_;
};

}