#pragma once

#include<map>
#include<deque>
#include<memory>
#include<unordered_map>
#include<vector>
#include"order.hpp"
#include"exceptions.hpp"

namespace matching_engine{

class OrderBook{
public:
    void addOrder(std::shared_ptr<Order> order);

    // Восстановление: помещает уже готовую заявку прямо в книгу, минуя
    // сопоставление (REQ-REC-04). Заявки нужно подавать в порядке возрастания
    // sequence_number, чтобы очередь внутри ценового уровня совпадала с
    // приоритетом по времени, который был до перезапуска.
    void restore(std::shared_ptr<Order> order);

    void removeOrder(int id);
    std::shared_ptr<Order> findOrder(int id) const;
    std::shared_ptr<Order> bestBuy() const;
    std::shared_ptr<Order> bestSell() const;
    std::vector<std::shared_ptr<Order>> buyOrders() const;
    std::vector<std::shared_ptr<Order>> sellOrders() const;
    bool empty() const noexcept;

private:
    std::map<int, std::deque<std::shared_ptr<Order>>> buyLevels_;
    std::map<int, std::deque<std::shared_ptr<Order>>> sellLevels_;
    std::unordered_map<int, std::shared_ptr<Order>> ordersById_;

    void removeFromLevels(std::map<int, std::deque<std::shared_ptr<Order>>>& levels,
        int price, int id);
};

}