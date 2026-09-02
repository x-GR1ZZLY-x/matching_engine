#include<algorithm>
#include"matching_engine.hpp"
#include"logger.hpp"
#include"exceptions.hpp"

namespace matching_engine{

ExecutionResult MatchingEngine::process(const Command& command){
    switch(command.type_){
        case CommandType::Add:
            if (const auto* market = dynamic_cast<const MarketAddCommand*>(&command)) {
                return processMarketAdd(*market);
            } else {
                return processAdd(static_cast<const AddCommand&>(command));
            }
        case CommandType::Cancel:
            return processCancel(static_cast<const CancelCommand&>(command));
        case CommandType::Print:
            return ExecutionResult{};
        case CommandType::Modify:
            return processModify(static_cast<const ModifyCommand&>(command));
    }
    return ExecutionResult{};
}

const std::vector<Trade>& MatchingEngine::trades() const noexcept{
    return trades_;
}

const OrderBook& MatchingEngine::orderBook() const noexcept{
    return orderBook_;
}

void MatchingEngine::restore(std::shared_ptr<Order> order){
    orderBook_.restore(order);
}

OrderChange MatchingEngine::snapshot(const Order& order){
    return OrderChange{
        order.getId(),
        order.getSide(),
        std::optional<int>(order.getPrice()),
        order.getInitialQuantity(),
        order.getQuantity(),
        order.getStatus(),
        order.getSequenceNumber()
    };
}

OrderChange MatchingEngine::snapshot(const MarketOrder& order){
    return OrderChange{
        order.getId(),
        order.getSide(),
        std::nullopt,
        order.getInitialQuantity(),
        order.getQuantity(),
        order.getStatus(),
        order.getSequenceNumber()
    };
}

ExecutionResult MatchingEngine::processAdd(const AddCommand& cmd){
    Logger::instance().info(
        "Order received: id=" + std::to_string(cmd.id_) +
        " side=" + Order::sideToString(cmd.side_) +
        " price=" + std::to_string(cmd.price_) +
        " quantity=" + std::to_string(cmd.quantity_)
    );

    if(orderBook_.findOrder(cmd.id_) != nullptr){
        throw DuplicateOrderError("Order with id " +
            std::to_string(cmd.id_) + " already exists");
    }

    auto order = std::make_shared<Order>(cmd.id_, cmd.side_, cmd.price_, cmd.quantity_);
    auto result = match(order);

    if(!order->isFilled()){
        orderBook_.addOrder(order);
        Logger::instance().info("Order added to book: id=" +
            std::to_string(cmd.id_));
    }else{
        Logger::instance().info(
            "Order completely filled on arrival: id=" + std::to_string(cmd.id_)
        );
    }

    result.orderChanges.push_back(snapshot(*order));

    for(auto& trade : result.trades){
        trades_.push_back(trade);
    }

    return result;
}

ExecutionResult MatchingEngine::processCancel(const CancelCommand& cmd){
    auto order = orderBook_.findOrder(cmd.id_);
    if(order == nullptr){
        throw OrderBookError("Cannot cancel: order with id " +
            std::to_string(cmd.id_) + " not found");
    }

    order->cancel();

    ExecutionResult result;
    result.orderChanges.push_back(snapshot(*order));

    orderBook_.removeOrder(cmd.id_);
    Logger::instance().info("Order cancelled: id=" +
        std::to_string(cmd.id_));

    return result;
}

ExecutionResult MatchingEngine::match(std::shared_ptr<Order> incoming){
    ExecutionResult result;

    while(!incoming->isFilled()){
        std::shared_ptr<Trade> trade;

        if(incoming->getSide() == Side::Buy){
            trade = tryMatchBuy(incoming, result.orderChanges);
        }else{
            trade = tryMatchSell(incoming, result.orderChanges);
        }

        if(!trade){
            break;
        }

        result.trades.push_back(*trade);
    }
    return result;
}

std::shared_ptr<Trade> MatchingEngine::tryMatchBuy(std::shared_ptr<Order> buyOrder,
    std::vector<OrderChange>& changes){
    auto bestSell = orderBook_.bestSell();
    if(!bestSell){
        return nullptr;
    }
    if(bestSell->getPrice() > buyOrder->getPrice()){
        return nullptr;
    }

    auto trade = executeTrade(bestSell, buyOrder, changes);

    return std::make_shared<Trade>(trade);
}

std::shared_ptr<Trade> MatchingEngine::tryMatchSell(std::shared_ptr<Order> sellOrder,
    std::vector<OrderChange>& changes){
    auto bestBuy = orderBook_.bestBuy();
    if(!bestBuy){
        return nullptr;
    }

    if(bestBuy->getPrice() < sellOrder->getPrice()){
        return nullptr;
    }

    auto trade = executeTrade(bestBuy, sellOrder, changes);

    return std::make_shared<Trade>(trade);
}

Trade MatchingEngine::executeTrade(std::shared_ptr<Order> bookOrder, std::shared_ptr<Order> incomingOrder,
    std::vector<OrderChange>& changes){
    const int tradePrice = bookOrder->getPrice();
    const int tradeQty = std::min(bookOrder->getQuantity(), incomingOrder->getQuantity());
    const int buyId = (bookOrder->getSide() == Side::Buy)
        ? bookOrder->getId() : incomingOrder->getId();

    const int sellId = (bookOrder->getSide() == Side::Sell)
        ? bookOrder->getId() : incomingOrder->getId();

    bookOrder->fill(tradeQty);
    incomingOrder->fill(tradeQty);

    Logger::instance().info(
        "Trade executed: buy=" + std::to_string(buyId) +
        " sell=" + std::to_string(sellId) +
        " price=" + std::to_string(tradePrice) +
        " qty=" + std::to_string(tradeQty)
    );

    // Снимок книжной заявки снимается сразу после fill, пока статус уже
    // обновлён, но заявка ещё не удалена из книги.
    changes.push_back(snapshot(*bookOrder));

    if(bookOrder->isFilled()){
        Logger::instance().info(
            "Order completely filled: id=" + std::to_string(bookOrder->getId())
        );
        orderBook_.removeOrder(bookOrder->getId());
    }else{
        Logger::instance().info(
            "Order partially filled: id=" + std::to_string(bookOrder->getId()) +
            " remaining=" + std::to_string(bookOrder->getQuantity())
        );
    }

    if(incomingOrder->isFilled()){
        Logger::instance().info(
            "Order completely filled: id=" + std::to_string(incomingOrder->getId())
        );
    }else{
        Logger::instance().info(
            "Order partially filled: id=" + std::to_string(incomingOrder->getId()) +
            " remaining=" + std::to_string(incomingOrder->getQuantity())
        );
    }

    return Trade(buyId, sellId, tradePrice, tradeQty);
}


ExecutionResult MatchingEngine::processMarketAdd(const MarketAddCommand& cmd) {
    Logger::instance().info(
        "Market order received: id=" + std::to_string(cmd.id_) +
        " side=" + Order::sideToString(cmd.side_) +
        " quantity=" + std::to_string(cmd.quantity_)
    );

    if (orderBook_.findOrder(cmd.id_) != nullptr) {
        throw DuplicateOrderError("Order with id " +
            std::to_string(cmd.id_) + " already exists");
    }

    auto order = std::make_shared<MarketOrder>(cmd.id_, cmd.side_, cmd.quantity_);
    auto result = matchMarket(order);

    for (auto& trade : result.trades) {
        trades_.push_back(trade);
    }

    // Остаток рыночной заявки в книгу не попадает, поэтому заявка,
    // исполненная не полностью (в том числе не исполненная вовсе),
    // фиксируется как отменённая — иначе она осела бы в снимке со
    // статусом OPEN/PARTIALLY_FILLED без цены, что ломает восстановление книги.
    if (!order->isFilled()) {
        order->cancel();
    }

    result.orderChanges.push_back(snapshot(*order));

    Logger::instance().info(
        "Market order processed: id=" + std::to_string(cmd.id_) +
        " filled=" + std::to_string(cmd.quantity_ - order->getQuantity()) +
        " remaining=" + std::to_string(order->getQuantity()) +
        " status=" + Order::statusToString(order->getStatus())
    );

    return result;
}

ExecutionResult MatchingEngine::matchMarket(std::shared_ptr<MarketOrder> incoming) {
    ExecutionResult result;

    while (!incoming->isFilled()) {
        std::shared_ptr<Trade> trade;

        if (incoming->getSide() == Side::Buy) {
            trade = tryMatchBuyMarket(incoming, result.orderChanges);
        } else {
            trade = tryMatchSellMarket(incoming, result.orderChanges);
        }

        if (!trade) break;

        result.trades.push_back(*trade);
    }
    return result;
}

std::shared_ptr<Trade> MatchingEngine::tryMatchBuyMarket(std::shared_ptr<MarketOrder> buyOrder,
    std::vector<OrderChange>& changes) {
    auto bestSell = orderBook_.bestSell();
    if (!bestSell) return nullptr;

    auto trade = executeMarketTrade(bestSell, buyOrder, changes);
    return std::make_shared<Trade>(trade);
}

std::shared_ptr<Trade> MatchingEngine::tryMatchSellMarket(std::shared_ptr<MarketOrder> sellOrder,
    std::vector<OrderChange>& changes) {
    auto bestBuy = orderBook_.bestBuy();
    if (!bestBuy) return nullptr;

    auto trade = executeMarketTrade(bestBuy, sellOrder, changes);
    return std::make_shared<Trade>(trade);
}

Trade MatchingEngine::executeMarketTrade(
    std::shared_ptr<Order> bookOrder,
    std::shared_ptr<MarketOrder> incomingOrder,
    std::vector<OrderChange>& changes)
{
    const int tradePrice = bookOrder->getPrice();
    const int tradeQty   = std::min(bookOrder->getQuantity(), incomingOrder->getQuantity());

    const int buyId  = (bookOrder->getSide() == Side::Buy)
        ? bookOrder->getId() : incomingOrder->getId();
    const int sellId = (bookOrder->getSide() == Side::Sell)
        ? bookOrder->getId() : incomingOrder->getId();

    bookOrder->fill(tradeQty);
    incomingOrder->fill(tradeQty);

    Logger::instance().info(
        "Trade executed: buy=" + std::to_string(buyId) +
        " sell=" + std::to_string(sellId) +
        " price=" + std::to_string(tradePrice) +
        " qty=" + std::to_string(tradeQty)
    );

    // Снимок книжной заявки — сразу после fill, до удаления из книги.
    changes.push_back(snapshot(*bookOrder));

    if (bookOrder->isFilled()) {
        Logger::instance().info("Order completely filled: id=" +
            std::to_string(bookOrder->getId()));
        orderBook_.removeOrder(bookOrder->getId());
    } else {
        Logger::instance().info(
            "Order partially filled: id=" + std::to_string(bookOrder->getId()) +
            " remaining=" + std::to_string(bookOrder->getQuantity())
        );
    }

    return Trade(buyId, sellId, tradePrice, tradeQty);
}

ExecutionResult MatchingEngine::processModify(const ModifyCommand& cmd) {
    auto order = orderBook_.findOrder(cmd.id_);
    if (!order) {
        throw OrderBookError("Cannot modify: order with id " +
            std::to_string(cmd.id_) + " not found");
    }

    const Side side = order->getSide();

    // Снимок старой заявки снимается до удаления из книги — иначе исходное
    // количество и номер в последовательности исчезли бы безвозвратно.
    const OrderChange oldSnapshot = snapshot(*order);

    orderBook_.removeOrder(cmd.id_);

    Logger::instance().info(
        "Order modified: id=" + std::to_string(cmd.id_) +
        " new_price=" + std::to_string(cmd.price_) +
        " new_qty=" + std::to_string(cmd.quantity_)
    );

    auto modified = std::make_shared<Order>(cmd.id_, side, cmd.price_, cmd.quantity_);
    auto result = match(modified);

    if (!modified->isFilled()) {
        orderBook_.addOrder(modified);
    }

    for (auto& trade : result.trades) {
        trades_.push_back(trade);
    }

    result.orderChanges.insert(result.orderChanges.begin(), oldSnapshot);
    result.orderChanges.push_back(snapshot(*modified));

    return result;
}

}
