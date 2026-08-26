#pragma once

#include <string>

namespace matching_engine{

struct DatabaseConfig{
    std::string host;
    std::string port;
    std::string dbname;
    std::string user;
    std::string password;
};

// Читает host/port/dbname/user из JSON-файла configPath и добавляет пароль
// из переменной окружения MATCHING_ENGINE_DB_PASSWORD (в файле пароля нет).
// Бросает ConfigError, если файл не найден, JSON невалиден, отсутствует
// обязательное поле или переменная окружения не задана либо пуста.
DatabaseConfig loadDatabaseConfig(const std::string& configPath);

}
