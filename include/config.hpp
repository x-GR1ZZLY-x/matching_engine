#pragma once

#include <cstddef>
#include <string>

namespace matching_engine{

struct ServerConfig{
    std::string address;
    int port = 0;
    std::size_t maxMessageSize = 0;
};

struct DatabaseConfig{
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
};

struct AppConfig{
    ServerConfig server;
    DatabaseConfig database;
};

// Читает секции "server" и "database" из JSON-файла configPath и добавляет
// пароль базы из переменной окружения MATCHING_ENGINE_DB_PASSWORD (в файле
// пароля нет). Бросает ConfigError, если файл не найден, JSON невалиден,
// отсутствует обязательная секция или поле, либо переменная окружения не
// задана или пуста.
AppConfig loadConfig(const std::string& configPath);

}
