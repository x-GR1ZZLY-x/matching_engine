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

    const nlohmann::json& requireField(const nlohmann::json& json, const std::string& field) const;
    int requireInt(const nlohmann::json& json, const std::string& field) const;
    std::string requireString(const nlohmann::json& json, const std::string& field) const;
};

}