#include<algorithm>
#include"order_book.hpp"

namespace matching_engine{

void OrderBook::addOrder(std::shared_ptr<Order> order){
    const int id = order->getId();

    if(ordersById_.count(id) > 0){
        throw DuplicateOrderError("Order with this id exists");
    }

    ordersById_[id] = order;

    if(order->getSide() == Side::Buy){
        buyLevels_[order->getPrice()].push_back(order);
    }else{
        sellLevels_[order->getPrice()].push_back(order);
    }
}

void OrderBook::restore(std::shared_ptr<Order> order){
    // Та же мутация обеих внутренних структур, что и addOrder — здесь нет
    // отдельного алгоритма, единственная разница в намерении вызывающего
    // кода: заявка уже стояла в книге до перезапуска и сопоставлению не
    // подлежит.
    addOrder(order);
}

void OrderBook::removeFromLevels(
    std::map<int, std::deque<std::shared_ptr<Order>>>& levels,
    int price,
    int id){

        auto levelIt = levels.find(price);
        if(levelIt == levels.end()) {
            return;
        }

        auto& deq = levelIt->second;
        deq.erase(
            std::remove_if(deq.begin(), deq.end(),
            [id](const std::shared_ptr<Order>& o){
                return o->getId() == id;
            }),
            deq.end()
        );

        if(deq.empty()){
            levels.erase(levelIt);
        }
}

void OrderBook::removeOrder(int id){
    auto it = ordersById_.find(id);
    if(it == ordersById_.end()){
        throw OrderBookError("Order with id " + std::to_string(id) + " not found");
    }

    const auto& order = it->second;
    const Side side = order->getSide();
    const int price = order->getPrice();

    if(side == Side::Buy){
        removeFromLevels(buyLevels_, price, id);
    }else{
        removeFromLevels(sellLevels_, price, id);
    }

    ordersById_.erase(it);
}

std::shared_ptr<Order> OrderBook:: findOrder(int id) const{
    auto it = ordersById_.find(id);
    if(it == ordersById_.end()){
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<Order> OrderBook::bestBuy() const{
    if(buyLevels_.empty()){
        return nullptr;
    }

    const auto& deq = buyLevels_.rbegin()->second;
    return deq.empty() ? nullptr : deq.front();
}

std::shared_ptr<Order> OrderBook::bestSell() const{
    if(sellLevels_.empty()){
        return nullptr;
    }

    const auto& deq = sellLevels_.begin()->second;
    return deq.empty() ? nullptr : deq.front();
}

std::vector<std::shared_ptr<Order>> OrderBook::buyOrders() const{
    std::vector<std::shared_ptr<Order>> result;

    for(auto it = buyLevels_.rbegin(); it != buyLevels_.rend(); ++it){
        for(const auto& order : it->second){
            result.push_back(order);
        }
    }
    return result;
}

std::vector<std::shared_ptr<Order>> OrderBook::sellOrders() const{
    std::vector<std::shared_ptr<Order>> result;

    for(const auto& [price, deq] : sellLevels_){
        for(const auto& order : deq){
            result.push_back(order);
        }
    }
    return result;
}

bool OrderBook::empty() const noexcept{
    return ordersById_.empty();
}

}