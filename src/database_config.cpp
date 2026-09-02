#include"database_config.hpp"
#include"exceptions.hpp"
#include<cstdlib>
#include<fstream>
#include<nlohmann/json.hpp>

namespace matching_engine{

namespace{

std::string requireString(const nlohmann::json& j, const std::string& field,
    const std::string& configPath){

    if(!j.contains(field)){
        throw ConfigError("Config file '" + configPath + "' is missing required field: " + field);
    }
    const auto& value = j.at(field);
    if(!value.is_string()){
        throw ConfigError("Config file '" + configPath + "' field '" + field + "' must be a string");
    }
    return value.get<std::string>();
}

}

DatabaseConfig loadDatabaseConfig(const std::string& configPath){
    std::ifstream file(configPath);
    if(!file.is_open()){
        throw ConfigError("Cannot open database config file: " + configPath);
    }

    nlohmann::json root;
    try{
        file >> root;
    } catch(const nlohmann::json::parse_error& e){
        throw ConfigError("Invalid JSON in config file '" + configPath + "': " + e.what());
    }

    DatabaseConfig config;
    config.host = requireString(root, "host", configPath);
    config.port = requireString(root, "port", configPath);
    config.dbname = requireString(root, "dbname", configPath);
    config.user = requireString(root, "user", configPath);

    const char* password = std::getenv("MATCHING_ENGINE_DB_PASSWORD");
    if(!password || password[0] == '\0'){
        throw ConfigError("Environment variable MATCHING_ENGINE_DB_PASSWORD is not set or empty");
    }
    config.password = password;

    return config;
}

}
