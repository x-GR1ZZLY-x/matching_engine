#include<algorithm>
#include"matching_engine.hpp"
#include"logger.hpp"
#include"exceptions.hpp"

namespace matching_engine{

void MatchingEngine::process(const Command& command){
    switch(command.type_){
        case CommandType::Add:
            if (const auto* market = dynamic_cast<const MarketAddCommand*>(&command)) {
                processMarketAdd(*market);
            } else {
                processAdd(static_cast<const AddCommand&>(command));
            }
            break;
        case CommandType::Cancel:
            processCancel(static_cast<const CancelCommand&>(command));
            break;
        case CommandType::Print:
            break;
        case CommandType::Modify:
            processModify(static_cast<const ModifyCommand&>(command));
            break;
    }
}

const std::vector<Trade>& MatchingEngine::trades() const noexcept{
    return trades_;
}

const OrderBook& MatchingEngine::orderBook() const noexcept{
    return orderBook_;
}


void MatchingEngine::processAdd(const AddCommand& cmd){
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
    auto newTrades = match(order);

    if(!order->isFilled()){
        orderBook_.addOrder(order);
        Logger::instance().info("Order added to book: id=" +
            std::to_string(cmd.id_));
    }else{
        Logger::instance().info(
            "Order completely filled on arrival: id=" + std::to_string(cmd.id_)
        );
    }

    for(auto& trade : newTrades){
        trades_.push_back(trade);
    }
}

void MatchingEngine::processCancel(const CancelCommand& cmd){
    auto order = orderBook_.findOrder(cmd.id_);
    if(order == nullptr){
        throw OrderBookError("Cannot cancel: order with id " +
            std::to_string(cmd.id_) + " not found");
    }

    orderBook_.removeOrder(cmd.id_);
    Logger::instance().info("Order cancelled: id=" +
        std::to_string(cmd.id_));
}

std::vector<Trade> MatchingEngine::match(std::shared_ptr<Order> incoming){
    std::vector<Trade> result;

    while(!incoming->isFilled()){
        std::shared_ptr<Trade> trade;

        if(incoming->getSide() == Side::Buy){
            trade = tryMatchBuy(incoming);
        }else{
            trade = tryMatchSell(incoming);
        }

        if(!trade){
            break;
        }

        result.push_back(*trade);
    }
    return result;
}

std::shared_ptr<Trade> MatchingEngine::tryMatchBuy(std::shared_ptr<Order> buyOrder){
    auto bestSell = orderBook_.bestSell();
    if(!bestSell){
        return nullptr;
    }
    if(bestSell->getPrice() > buyOrder->getPrice()){
        return nullptr;
    }

    auto trade = executeTrade(bestSell, buyOrder);

    return std::make_shared<Trade>(trade);
}

std::shared_ptr<Trade> MatchingEngine::tryMatchSell(std::shared_ptr<Order> sellOrder){
    auto bestBuy = orderBook_.bestBuy();
    if(!bestBuy){
        return nullptr;
    }

    if(bestBuy->getPrice() < sellOrder->getPrice()){
        return nullptr;
    }

    auto trade = executeTrade(bestBuy, sellOrder);

    return std::make_shared<Trade>(trade);
}

Trade MatchingEngine::executeTrade(std::shared_ptr<Order> bookOrder, std::shared_ptr<Order> incomingOrder){
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


void MatchingEngine::processMarketAdd(const MarketAddCommand& cmd) {
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
    auto newTrades = matchMarket(order);

    for (auto& trade : newTrades) {
        trades_.push_back(trade);
    }

    Logger::instance().info(
        "Market order processed: id=" + std::to_string(cmd.id_) +
        " filled=" + std::to_string(cmd.quantity_ - order->getQuantity()) +
        " remaining=" + std::to_string(order->getQuantity())
    );
}

std::vector<Trade> MatchingEngine::matchMarket(std::shared_ptr<MarketOrder> incoming) {
    std::vector<Trade> result;

    while (!incoming->isFilled()) {
        std::shared_ptr<Trade> trade;

        if (incoming->getSide() == Side::Buy) {
            trade = tryMatchBuyMarket(incoming);
        } else {
            trade = tryMatchSellMarket(incoming);
        }

        if (!trade) break;

        result.push_back(*trade);
    }
    return result;
}

std::shared_ptr<Trade> MatchingEngine::tryMatchBuyMarket(std::shared_ptr<MarketOrder> buyOrder) {
    auto bestSell = orderBook_.bestSell();
    if (!bestSell) return nullptr;

    auto trade = executeMarketTrade(bestSell, buyOrder);
    return std::make_shared<Trade>(trade);
}

std::shared_ptr<Trade> MatchingEngine::tryMatchSellMarket(std::shared_ptr<MarketOrder> sellOrder) {
    auto bestBuy = orderBook_.bestBuy();
    if (!bestBuy) return nullptr;

    auto trade = executeMarketTrade(bestBuy, sellOrder);
    return std::make_shared<Trade>(trade);
}

Trade MatchingEngine::executeMarketTrade(
    std::shared_ptr<Order> bookOrder,
    std::shared_ptr<MarketOrder> incomingOrder)
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

void MatchingEngine::processModify(const ModifyCommand& cmd) {
    auto order = orderBook_.findOrder(cmd.id_);
    if (!order) {
        throw OrderBookError("Cannot modify: order with id " +
            std::to_string(cmd.id_) + " not found");
    }

    const Side side = order->getSide();

    orderBook_.removeOrder(cmd.id_);

    Logger::instance().info(
        "Order modified: id=" + std::to_string(cmd.id_) +
        " new_price=" + std::to_string(cmd.price_) +
        " new_qty=" + std::to_string(cmd.quantity_)
    );

    auto modified = std::make_shared<Order>(cmd.id_, side, cmd.price_, cmd.quantity_);
    auto newTrades = match(modified);

    if (!modified->isFilled()) {
        orderBook_.addOrder(modified);
    }

    for (auto& trade : newTrades) {
        trades_.push_back(trade);
    }
}

}