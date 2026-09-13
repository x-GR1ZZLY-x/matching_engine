#include"config.hpp"
#include"exceptions.hpp"
#include"response_serializer.hpp"
#include<cstdint>
#include<cstdlib>
#include<fstream>
#include<limits>
#include<string>
#include<nlohmann/json.hpp>

namespace matching_engine{

// Нижняя граница server.max_message_size (docs/task4/02-network-protocol.md,
// раздел 4.1). Решение "ответ, который не поместился, отвечает
// RESPONSE_TOO_LARGE и держит соединение живым" работает, только если
// короткий ответ об ошибке помещается в лимит всегда — иначе сервер не мог
// бы сообщить об ошибке вообще. Значение берётся из
// ResponseSerializer::maxErrorResponseSize() — она вызывает настоящую
// error() на заведомо худшем входе, а не собирает JSON вручную здесь: так
// граница не может разойтись с тем, что реально шлёт сериализатор при
// следующей правке его формата.
std::size_t minMaxMessageSize(){
    return ResponseSerializer::maxErrorResponseSize();
}

namespace{

const nlohmann::json& requireSection(const nlohmann::json& root, const std::string& section,
    const std::string& configPath){

    if(!root.contains(section)){
        throw ConfigError("Config file '" + configPath + "' is missing required section: " +
            section);
    }
    const auto& value = root.at(section);
    if(!value.is_object()){
        throw ConfigError("Config file '" + configPath + "' section '" + section +
            "' must be an object");
    }
    return value;
}

std::string requireString(const nlohmann::json& section, const std::string& sectionName,
    const std::string& field, const std::string& configPath){

    if(!section.contains(field)){
        throw ConfigError("Config file '" + configPath + "' is missing required field: " +
            sectionName + "." + field);
    }
    const auto& value = section.at(field);
    if(!value.is_string()){
        throw ConfigError("Config file '" + configPath + "' field '" + sectionName + "." +
            field + "' must be a string");
    }
    return value.get<std::string>();
}

long long requireNonNegativeInt(const nlohmann::json& section, const std::string& sectionName,
    const std::string& field, const std::string& configPath){

    if(!section.contains(field)){
        throw ConfigError("Config file '" + configPath + "' is missing required field: " +
            sectionName + "." + field);
    }
    const auto& value = section.at(field);
    if(!value.is_number_integer() || value.get<long long>() < 0){
        throw ConfigError("Config file '" + configPath + "' field '" + sectionName + "." +
            field + "' must be a non-negative integer");
    }
    return value.get<long long>();
}

// Порт, в отличие от произвольного неотрицательного целого, ограничен сверху
// диапазоном TCP-портов; ноль — законное значение (порт выбирает ОС).
int requirePort(const nlohmann::json& section, const std::string& sectionName,
    const std::string& field, const std::string& configPath){

    const long long value = requireNonNegativeInt(section, sectionName, field, configPath);
    if(value > 65535){
        throw ConfigError("Config file '" + configPath + "' field '" + sectionName + "." +
            field + "' must be between 0 and 65535");
    }
    return static_cast<int>(value);
}

// max_message_size ограничивает длину полезной нагрузки, которую
// MessageCodec::decodeHeader разбирает в uint32_t заголовка. Ноль запретил
// бы любое непустое сообщение, а значение больше UINT32_MAX не отловится ни
// при каком заголовке (заголовок сам не может объявить больше UINT32_MAX) и
// тихо снимет защиту от заявленного гиганта.
std::size_t requireMaxMessageSize(const nlohmann::json& section, const std::string& sectionName,
    const std::string& field, const std::string& configPath){

    const long long value = requireNonNegativeInt(section, sectionName, field, configPath);
    constexpr long long maxAllowed =
        static_cast<long long>(std::numeric_limits<std::uint32_t>::max());
    const long long minAllowed = static_cast<long long>(minMaxMessageSize());
    if(value < minAllowed || value > maxAllowed){
        throw ConfigError("Config file '" + configPath + "' field '" + sectionName + "." +
            field + "' must be between " + std::to_string(minAllowed) + " and " +
            std::to_string(maxAllowed));
    }
    return static_cast<std::size_t>(value);
}

}

AppConfig loadConfig(const std::string& configPath){
    std::ifstream file(configPath);
    if(!file.is_open()){
        throw ConfigError("Cannot open config file: " + configPath);
    }

    nlohmann::json root;
    try{
        file >> root;
    } catch(const nlohmann::json::parse_error& e){
        throw ConfigError("Invalid JSON in config file '" + configPath + "': " + e.what());
    }

    if(!root.is_object()){
        throw ConfigError("Config file '" + configPath + "' must contain a JSON object");
    }

    const auto& serverSection = requireSection(root, "server", configPath);
    const auto& databaseSection = requireSection(root, "database", configPath);

    AppConfig config;
    config.server.address = requireString(serverSection, "server", "address", configPath);
    config.server.port = requirePort(serverSection, "server", "port", configPath);
    config.server.maxMessageSize =
        requireMaxMessageSize(serverSection, "server", "max_message_size", configPath);

    config.database.host = requireString(databaseSection, "database", "host", configPath);
    config.database.port = std::to_string(
        requirePort(databaseSection, "database", "port", configPath));
    config.database.dbname = requireString(databaseSection, "database", "name", configPath);
    config.database.user = requireString(databaseSection, "database", "user", configPath);
    config.database.schemaDir =
        requireString(databaseSection, "database", "schema_dir", configPath);

    const char* password = std::getenv("MATCHING_ENGINE_DB_PASSWORD");
    if(!password || password[0] == '\0'){
        throw ConfigError("Environment variable MATCHING_ENGINE_DB_PASSWORD is not set or empty");
    }
    config.database.password = password;

    return config;
}

}
