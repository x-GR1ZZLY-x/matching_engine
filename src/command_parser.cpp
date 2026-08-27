#include "command_parser.hpp"
#include <nlohmann/json.hpp>

namespace matching_engine{

const nlohmann::json& CommandParser::requireField(
    const nlohmann::json& j,
    const std::string& field) const {
        if(!j.contains(field)){
            throw ParseError("Missed required field: " + field);
        }
        return j.at(field);
}


int CommandParser::requireInt(
    const nlohmann::json& j,
    const std::string& field) const {
        const auto& value = requireField(j, field);

        if(!value.is_number_integer()){
            throw ParseError("Field " + field + " must be an integer");
        }

        return value.get<int>();
}

std::string CommandParser::requireString(
    const nlohmann::json& j,
    const std::string& field) const {
        const auto& value = requireField(j, field);
        if(!value.is_string()){
            throw ParseError("Field '" + field + "' must be a string ");
        }
        return value.get<std::string>();
}

std::unique_ptr<Command> CommandParser::parseAdd(const nlohmann::json& j) const {
    if (j.contains("order_type")) {
        const std::string orderType = requireString(j, "order_type");
        if (orderType == "MARKET") {
            return parseMarketAdd(j);
        }
        throw ParseError("Unknown order_type: " + orderType);
    }

    const int id = requireInt(j, "id");
    const std::string sideStr = requireString(j, "side");
    const int price = requireInt(j, "price");
    const int quantity = requireInt(j, "quantity");

    Side side;
    try{
        side = Order::sideFromString(sideStr);
    } catch (const OrderError& e){
        throw ParseError(std::string("Invalid side value: ") + e.what());
    }

    if(id <= 0){
        throw ParseError("Id must be positive");
    }
    if(price <= 0){
        throw ParseError("Price must be positive");
    }
    if(quantity <= 0){
        throw ParseError("Quantity must be positive");
    }
    return std::make_unique<AddCommand>(id, side, price, quantity);
}

std::unique_ptr<Command> CommandParser::parseCancel(const nlohmann::json& j) const{
    const int id = requireInt(j, "id");
    if(id <= 0) {
        throw ParseError("Id must be positive");
    }
    return std::make_unique<CancelCommand>(id);
}

std::unique_ptr<Command> CommandParser::parse(const nlohmann::json& j) const{
    const std::string type = requireString(j, "type");

    std::unique_ptr<Command> command;
    if(type == "ADD") command = parseAdd(j);
    else if(type == "CANCEL") command = parseCancel(j);
    else if(type == "PRINT") command = std::make_unique<PrintCommand>();
    else if(type == "MODIFY") command = parseModify(j);
    else throw ParseError("Unknown command type");

    // command_id — необязательное поле на уровне парсера (задача 07, п.4):
    // обязательность для изменяющих команд проверяет CommandProcessor.
    if(j.contains("command_id")){
        command->commandId_ = requireString(j, "command_id");
    }

    return command;
}

std::unique_ptr<Command> CommandParser::parseMarketAdd(const nlohmann::json& j) const {
    const int id = requireInt(j, "id");
    const std::string sideStr = requireString(j, "side");
    const int quantity = requireInt(j, "quantity");

    Side side;
    try {
        side = Order::sideFromString(sideStr);
    } catch (const OrderError& e) {
        throw ParseError(std::string("Invalid side value: ") + e.what());
    }

    if (id <= 0)       throw ParseError("Id must be positive");
    if (quantity <= 0) throw ParseError("Quantity must be positive");

    return std::make_unique<MarketAddCommand>(id, side, quantity);
}

std::unique_ptr<Command> CommandParser::parseModify(const nlohmann::json& j) const {
    const int id       = requireInt(j, "id");
    const int price    = requireInt(j, "price");
    const int quantity = requireInt(j, "quantity");

    if (id <= 0)       throw ParseError("Id must be positive");
    if (price <= 0)    throw ParseError("Price must be positive");
    if (quantity <= 0) throw ParseError("Quantity must be positive");

    return std::make_unique<ModifyCommand>(id, price, quantity);
}

}