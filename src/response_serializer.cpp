#include "response_serializer.hpp"

#include <nlohmann/json.hpp>

namespace matching_engine {

namespace {

// Защитный предел на message: часть сообщений собирается из пользовательского
// ввода на глубине (например, CommandParser сообщает о недопустимом
// order_type буквальным текстом поля), и без верхней границы здесь
// длинное значение поля могло бы раздуть ответ так же, как неограниченный
// эхо command_id. Обычные сообщения этого проекта — короткие технические
// фразы, кратно короче предела.
constexpr std::size_t kMaxMessageLength = 512;

std::string truncateMessage(const std::string& message) {
    if (message.size() <= kMaxMessageLength) {
        return message;
    }
    return message.substr(0, kMaxMessageLength) + "...";
}

// Неправдоподобно длинный command_id отсеивается раньше, в RequestRouter
// (docs/task4/02-network-protocol.md, раздел 3.1/3.5), поэтому здесь эхо
// безусловно: если commandId дошёл до сериализатора, он уже прошёл проверку
// длины и должен попасть в ответ буквально.
void putCommandIdIfEligible(nlohmann::json& response,
    const std::optional<std::string>& commandId) {
    if (commandId) {
        response["command_id"] = *commandId;
    }
}

nlohmann::json bookEntry(int orderId, int price, int quantity) {
    nlohmann::json entry;
    entry["order_id"] = orderId;
    entry["price"] = price;
    entry["quantity"] = quantity;
    return entry;
}

}

std::string ResponseSerializer::success(const std::optional<std::string>& commandId,
    int orderId, const ExecutionResult& result) {
    nlohmann::json response;
    putCommandIdIfEligible(response, commandId);
    response["status"] = "OK";
    response["order_id"] = orderId;

    nlohmann::json trades = nlohmann::json::array();
    for (const auto& trade : result.trades) {
        nlohmann::json tradeJson;
        tradeJson["buy_order_id"] = trade.getBuyOrderId();
        tradeJson["sell_order_id"] = trade.getSellOrderId();
        tradeJson["price"] = trade.getPrice();
        tradeJson["quantity"] = trade.getQuantity();
        trades.push_back(tradeJson);
    }
    response["trades"] = trades;

    return response.dump();
}

std::string ResponseSerializer::error(const std::optional<std::string>& commandId,
    const std::string& code, const std::string& message) {
    nlohmann::json response;
    putCommandIdIfEligible(response, commandId);
    response["status"] = "ERROR";
    response["error"] = code;
    response["message"] = truncateMessage(message);
    return response.dump();
}

std::string ResponseSerializer::printBook(const OrderBook& book,
    const std::optional<std::string>& commandId) {
    nlohmann::json buy = nlohmann::json::array();
    for (const auto& order : book.buyOrders()) {
        buy.push_back(bookEntry(order->getId(), order->getPrice(), order->getQuantity()));
    }

    nlohmann::json sell = nlohmann::json::array();
    for (const auto& order : book.sellOrders()) {
        sell.push_back(bookEntry(order->getId(), order->getPrice(), order->getQuantity()));
    }

    nlohmann::json response;
    putCommandIdIfEligible(response, commandId);
    response["status"] = "OK";
    response["result"]["buy"] = buy;
    response["result"]["sell"] = sell;
    return response.dump();
}

}
