#include"command_processor.hpp"
#include<nlohmann/json.hpp>
#include"exceptions.hpp"

namespace matching_engine{

namespace{

// AddCommand и MarketAddCommand оба несут CommandType::Add ("Command: тип команды не однозначно определяет класс"), поэтому рыночная
// заявка записывается в processed_commands с command_type = 'ADD', а не
// отдельным значением вроде MARKET_ADD. Это осознанно: различение через
// dynamic_cast завело бы третью точку развилки «лимитная/рыночная», а
// восстановление кеша в задаче 08 читает не command_type, а колонку result.
std::string commandTypeToString(CommandType type){
    switch(type){
        case CommandType::Add:    return "ADD";
        case CommandType::Cancel: return "CANCEL";
        case CommandType::Modify: return "MODIFY";
        case CommandType::Print:  return "PRINT";
    }
    throw ParseError("Unknown command type");
}

// Единственный статус, с которым запись попадает в processed_commands: до
// этой точки код доходит только после успешного сопоставления и успешной
// записи в транзакции — любая доменная ошибка выбрасывается раньше и сюда
// не долетает. Задача просит не изобретать матрицу статусов, поэтому
// значение одно и фиксированное.
constexpr const char* kAppliedStatus = "APPLIED";

nlohmann::json orderChangeToJson(const OrderChange& change){
    nlohmann::json j;
    j["id"] = change.id;
    j["side"] = Order::sideToString(change.side);
    j["price"] = change.price ? nlohmann::json(*change.price) : nlohmann::json(nullptr);
    j["initial_quantity"] = change.initialQuantity;
    j["remaining_quantity"] = change.remainingQuantity;
    j["status"] = Order::statusToString(change.status);
    j["sequence_number"] = change.sequenceNumber;
    return j;
}

nlohmann::json tradeToJson(const Trade& trade){
    nlohmann::json j;
    j["buy_order_id"] = trade.getBuyOrderId();
    j["sell_order_id"] = trade.getSellOrderId();
    j["price"] = trade.getPrice();
    j["quantity"] = trade.getQuantity();
    return j;
}

// Сериализация в processed_commands.result (JSONB), обратимая по
// построению: каждое поле Trade/OrderChange отражено один в один, включая
// порядок orderChanges в массиве. Задача 08 сможет разобрать это обратно в
// ExecutionResult при прогреве кеша при старте; сам разбор обратно — уже
// задача 08 (см. границы задачи 07), здесь важно только не потерять
// информацию при записи.
std::string serializeResult(const ExecutionResult& result){
    nlohmann::json j;

    j["trades"] = nlohmann::json::array();
    for(const auto& trade : result.trades){
        j["trades"].push_back(tradeToJson(trade));
    }

    j["order_changes"] = nlohmann::json::array();
    for(const auto& change : result.orderChanges){
        j["order_changes"].push_back(orderChangeToJson(change));
    }

    return j.dump();
}

}

ExecutionResult CommandProcessor::process(const Command& command, PgConnection& connection){
    if(!command.commandId_ || command.commandId_->empty()){
        throw ParseError("command_id is required for a state-changing command (type " +
            commandTypeToString(command.type_) + ")");
    }
    const std::string& commandId = *command.commandId_;

    if(const ExecutionResult* cached = cache_.find(commandId)){
        // REQ-IDEM-02/03: команда уже обработана — возвращаем прошлый
        // результат, книгу не трогаем и в БД ничего не читаем и не пишем.
        return *cached;
    }

    ExecutionResult result = engine_.process(command);

    const ProcessedCommandRecord record{commandId, commandTypeToString(command.type_),
        kAppliedStatus, serializeResult(result)};
    persistence_.save(connection, result, record);

    cache_.put(commandId, result);
    return result;
}

const OrderBook& CommandProcessor::orderBook() const noexcept{
    return engine_.orderBook();
}

}
