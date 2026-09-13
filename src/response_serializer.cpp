#include "response_serializer.hpp"

#include <nlohmann/json.hpp>

namespace matching_engine {

namespace {

// Неправдоподобно длинный command_id обычно уже отсеян раньше, в
// RequestRouter (docs/task4/02-network-protocol.md, раздел 3.1/3.5). Длина
// проверяется здесь ещё раз, а не только у вызывающей стороны: на этом
// инварианте построен расчёт maxErrorResponseSize() ниже (и через него —
// нижняя граница server.max_message_size, config.cpp), и он обязан
// держаться в месте, которое его использует, а не только там, откуда
// вызывается сериализатор — замечание ревью второго круга, п.1: до этой
// правки RequestRouter проверял "type" раньше длины command_id, и запрос
// без "type" мог дойти сюда с неотфильтрованным эхом.
void putCommandIdIfEligible(nlohmann::json& response,
    const std::optional<std::string>& commandId) {
    if (commandId && commandId->size() <= ResponseSerializer::kMaxCommandIdLength) {
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

std::string ResponseSerializer::truncateUtf8(const std::string& value, std::size_t maxLength) {
    if (value.size() <= maxLength) {
        return value;
    }
    std::size_t cut = maxLength;
    // value[cut] — первый исключаемый байт. Пока он сам оказывается
    // продолжением последовательности (10xxxxxx), сдвигаем границу назад:
    // так мы находим байт, с которого начинается последовательность,
    // накрывающая границу maxLength, и исключаем её целиком. Если
    // value[cut] не продолжение (граница попала между символами или на
    // однобайтовый ASCII), цикл не выполняется ни разу — усечение по
    // целым символам не трогается.
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return value.substr(0, cut) + "...";
}

std::size_t ResponseSerializer::maxErrorResponseSize() {
    // Управляющий байт 0x01 (подойдёт любой C0-байт) — символ, который
    // nlohmann::json::dump() экранирует сильнее прочих: шесть символов на
    // один входной байт, больше, чем два байта на кавычку или обратный
    // слэш. message и command_id оба приходят из данных, которые сервер не
    // полностью контролирует (текст исключения, command_id клиента),
    // поэтому граница обязана учитывать именно это расширение, а не
    // количество байт до экранирования.
    const std::string worstCaseCommandId(kMaxCommandIdLength, static_cast<char>(1));
    // На один байт длиннее лимита — гарантирует, что error() пройдёт через
    // truncateUtf8() и добавит "...", как и в реальном худшем случае.
    const std::string worstCaseMessage(kMaxMessageLength + 1, static_cast<char>(1));
    return error(worstCaseCommandId, "RESPONSE_TOO_LARGE", worstCaseMessage).size();
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

    // error_handler_t::replace вместо умолчания (strict): невалидная UTF-8
    // последовательность не на границе усечения (сегодня недостижимо — эти
    // строки приходят из уже разобранного JSON, а не из сырых байт) иначе
    // заставила бы dump() бросить исключение, а оно в Session закрыло бы
    // соединение (замечание ревью, п.5).
    return response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string ResponseSerializer::error(const std::optional<std::string>& commandId,
    const std::string& code, const std::string& message) {
    nlohmann::json response;
    putCommandIdIfEligible(response, commandId);
    response["status"] = "ERROR";
    response["error"] = code;
    response["message"] = truncateUtf8(message, kMaxMessageLength);
    // error_handler_t::replace — см. комментарий в success() выше.
    return response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
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
    // error_handler_t::replace — см. комментарий в success() выше.
    return response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}
