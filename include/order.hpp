#pragma once

#include "exceptions.hpp"
#include <string>

namespace matching_engine{

enum class Side{
    Buy,
    Sell
};

class Order{
public:
    Order(int id, Side side, int price, int quantity);

    int getId() const noexcept{ return id_; }
    Side getSide() const noexcept{ return side_; }
    int getPrice() const noexcept{ return price_; }
    int getQuantity() const noexcept{ return quantity_; }

    void fill(int quantity);

    bool isFilled() const noexcept {return (quantity_ == 0); }

    static std::string sideToString(Side side);
    static Side sideFromString(const std::string& str);
private:
    int id_;
    Side side_;
    int price_;
    int quantity_;
};

}