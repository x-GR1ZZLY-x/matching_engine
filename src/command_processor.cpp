#include"command_processor.hpp"
#include<nlohmann/json.hpp>
#include"exceptions.hpp"
#include"logger.hpp"

namespace matching_engine{

namespace{

// AddCommand и MarketAddCommand оба несут CommandType::Add ("Command: тип
// команды не однозначно определяет класс"), поэтому рыночная
// заявка записывается в processed_commands с command_type = 'ADD', а не
// отдельным значением вроде MARKET_ADD. Это осознанно: различение через
// dynamic_cast завело бы третью точку развилки «лимитная/рыночная», а
// восстановление кеша читает не command_type, а колонку result.
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

// Обратная пара к orderChangeToJson/tradeToJson — тот же набор ключей,
// прочитанный назад. Живёт рядом с прямым направлением в одном файле:
// формат processed_commands.result описан здесь и только здесь,
// разъехавшиеся сериализация и разбор молча
// сломали бы прогрев кеша после рестарта.
OrderChange orderChangeFromJson(const nlohmann::json& j){
    OrderChange change;
    change.id = j.at("id").get<int>();
    change.side = Order::sideFromString(j.at("side").get<std::string>());
    change.price = j.at("price").is_null()
        ? std::nullopt
        : std::optional<int>(j.at("price").get<int>());
    change.initialQuantity = j.at("initial_quantity").get<int>();
    change.remainingQuantity = j.at("remaining_quantity").get<int>();
    change.status = Order::statusFromString(j.at("status").get<std::string>());
    change.sequenceNumber = j.at("sequence_number").get<long long>();
    return change;
}

Trade tradeFromJson(const nlohmann::json& j){
    return Trade(j.at("buy_order_id").get<int>(), j.at("sell_order_id").get<int>(),
        j.at("price").get<int>(), j.at("quantity").get<int>());
}

}

// Сериализация в processed_commands.result (JSONB), обратимая по
// построению: каждое поле Trade/OrderChange отражено один в один, включая
// порядок orderChanges в массиве.
std::string CommandProcessor::serializeResult(const ExecutionResult& result){
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

// Разбор JSON, испорченный библиотекой (не найден ключ, не тот тип) —
// nlohmann::json::exception, не входящий в иерархию MatchingEngineError;
// без перехвата такое исключение долетело бы до std::terminate тем же
// путём, которого нужно избегать. side/status,
// нарушающие свой формат, дают OrderError — он уже MatchingEngineError,
// оборачивать незачем.
ExecutionResult CommandProcessor::parseResult(const std::string& json){
    try{
        nlohmann::json j = nlohmann::json::parse(json);

        ExecutionResult result;
        for(const auto& tradeJson : j.at("trades")){
            result.trades.push_back(tradeFromJson(tradeJson));
        }
        for(const auto& changeJson : j.at("order_changes")){
            result.orderChanges.push_back(orderChangeFromJson(changeJson));
        }
        return result;
    } catch(const nlohmann::json::exception& e){
        throw DatabaseError(std::string("Corrupted processed_commands.result JSON: ") + e.what());
    }
}

ExecutionResult CommandProcessor::process(const Command& command, PgConnection& connection,
    bool* servedFromCache){
    if(!command.commandId_ || command.commandId_->empty()){
        throw ParseError("command_id is required for a state-changing command (type " +
            commandTypeToString(command.type_) + ")");
    }
    const std::string& commandId = *command.commandId_;

    if(const ExecutionResult* cached = cache_.find(commandId)){
        // REQ-IDEM-02/03: команда уже обработана — возвращаем прошлый
        // результат, книгу не трогаем и в БД ничего не читаем и не пишем.
        if(servedFromCache){
            *servedFromCache = true;
        }
        return *cached;
    }

    ExecutionResult result = engine_.process(command);

    const ProcessedCommandRecord record{commandId, commandTypeToString(command.type_),
        kAppliedStatus, serializeResult(result)};
    persistence_.save(connection, result, record);

    cache_.put(commandId, result);
    if(servedFromCache){
        *servedFromCache = false;
    }
    return result;
}

ExecutionResult CommandProcessor::processBatched(const Command& command, bool* servedFromCache){
    if(!command.commandId_ || command.commandId_->empty()){
        throw ParseError("command_id is required for a state-changing command (type " +
            commandTypeToString(command.type_) + ")");
    }
    const std::string& commandId = *command.commandId_;

    if(const ExecutionResult* cached = cache_.find(commandId)){
        // Тот же ранний возврат, что и в process(): команда уже отработана
        // (в этом пакете или раньше), книгу не трогаем и в буфер ничего не
        // добавляем.
        if(servedFromCache){
            *servedFromCache = true;
        }
        return *cached;
    }

    ExecutionResult result = engine_.process(command);

    ProcessedCommandRecord record{commandId, commandTypeToString(command.type_),
        kAppliedStatus, serializeResult(result)};

    // Кеш заполняется сразу, а не после flushBatch: иначе повтор command_id внутри одного
    // пакета не был бы опознан и команда выполнилась бы дважды.
    cache_.put(commandId, result);

    pendingResults_.push_back(result);
    pendingRecords_.push_back(std::move(record));

    if(servedFromCache){
        *servedFromCache = false;
    }
    return result;
}

void CommandProcessor::flushBatch(PgConnection& connection){
    if(pendingRecords_.empty()){
        return;
    }
    persistence_.saveBatch(connection, pendingResults_, pendingRecords_);
    pendingResults_.clear();
    pendingRecords_.clear();
}

std::size_t CommandProcessor::pendingCount() const noexcept{
    return pendingRecords_.size();
}

const OrderBook& CommandProcessor::orderBook() const noexcept{
    return engine_.orderBook();
}

void CommandProcessor::restoreOrder(std::shared_ptr<Order> order){
    engine_.restore(std::move(order));
}

void CommandProcessor::warmCache(const std::string& commandId,
    const std::optional<std::string>& resultJson){
    if(!resultJson){
        // Колонка nullable — NULL сюда попадает не от нормального процесса
        // сохранения результата, а от внешнего вмешательства (ручная правка, сторонний
        // инструмент). Молча подставлять пустой результат не запрещено, но
        // след в логе обязателен: иначе повтор этой команды после рестарта
        // тихо ответит "сделок нет" вместо прошлого результата.
        Logger::instance().warning("processed_commands.result is NULL for command '" +
            commandId + "', warming cache with an empty result");
        cache_.put(commandId, ExecutionResult{});
        return;
    }

    try{
        cache_.put(commandId, parseResult(*resultJson));
    } catch(const MatchingEngineError& e){
        throw DatabaseError("Corrupted result for command '" + commandId + "': " + e.what());
    }
}

}
