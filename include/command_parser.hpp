#pragma once

#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "command.hpp"
#include "exceptions.hpp"

namespace matching_engine{
    
class CommandParser{
public:
    std::unique_ptr<Command> parse(const nlohmann::json& json) const;
private:
    std::unique_ptr<Command> parseAdd(const nlohmann::json& json) const;
    std::unique_ptr<Command> parseCancel(const nlohmann::json& json) const;
    std::unique_ptr<Command> parseMarketAdd(const nlohmann::json& json) const;
    std::unique_ptr<Command> parseModify(const nlohmann::json& json) const;

    const nlohmann::json& requireField(const nlohmann::json& json, const std::string& field) const;
    int requireInt(const nlohmann::json& json, const std::string& field) const;
    std::string requireString(const nlohmann::json& json, const std::string& field) const;

    // Идентификатор заявки передаётся полем "order_id" (REQ-API-10,
    // docs/task4/02-network-protocol.md, раздел 2); "id" принимается как
    // синоним ради совместимости с консольным режимом прошлых работ и его
    // тестами. "order_id" проверяется первым — если оба поля почему-то
    // присутствуют одновременно, побеждает имя, заданное сетевым API.
    int requireOrderId(const nlohmann::json& json) const;
};

}