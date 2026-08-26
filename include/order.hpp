#pragma once

#include "exceptions.hpp"
#include <string>

namespace matching_engine{

enum class Side{
    Buy,
    Sell
};

enum class OrderStatus{
    Open,
    PartiallyFilled,
    Filled,
    Cancelled
};

class Order{
public:
    Order(int id, Side side, int price, int quantity);

    // Восстанавливающий конструктор: собирает заявку из строки БД целиком,
    // не трогая SequenceGenerator — иначе восстановленная заявка получила бы
    // свежий номер последовательности и потеряла бы приоритет по времени.
    Order(int id, Side side, int price, int quantity,
          int initialQuantity, long long sequenceNumber, OrderStatus status);

    int getId() const noexcept{ return id_; }
    Side getSide() const noexcept{ return side_; }
    int getPrice() const noexcept{ return price_; }
    int getQuantity() const noexcept{ return quantity_; }
    int getInitialQuantity() const noexcept{ return initialQuantity_; }
    long long getSequenceNumber() const noexcept{ return sequenceNumber_; }
    OrderStatus getStatus() const noexcept{ return status_; }

    void fill(int quantity);
    void cancel();

    bool isFilled() const noexcept {return (quantity_ == 0); }

    static std::string sideToString(Side side);
    static Side sideFromString(const std::string& str);

    static std::string statusToString(OrderStatus status);
    static OrderStatus statusFromString(const std::string& str);
private:
    int id_;
    Side side_;
    int price_;
    int quantity_;
    int initialQuantity_;
    long long sequenceNumber_;
    OrderStatus status_;
};

class MarketOrder {
public:
    MarketOrder(int id, Side side, int quantity);

    // Восстанавливающий конструктор — см. пояснение у Order.
    MarketOrder(int id, Side side, int quantity,
                int initialQuantity, long long sequenceNumber, OrderStatus status);

    int getId()      const noexcept { return id_; }
    Side getSide()   const noexcept { return side_; }
    int getQuantity() const noexcept { return quantity_; }
    int getInitialQuantity() const noexcept { return initialQuantity_; }
    long long getSequenceNumber() const noexcept { return sequenceNumber_; }
    OrderStatus getStatus() const noexcept { return status_; }

    void fill(int quantity);
    void cancel();
    bool isFilled() const noexcept { return quantity_ == 0; }

private:
    int id_;
    Side side_;
    int quantity_;
    int initialQuantity_;
    long long sequenceNumber_;
    OrderStatus status_;
};

}
