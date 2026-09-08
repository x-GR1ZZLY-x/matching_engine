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

    // Каталог с файлами схемы (*.sql), применяемыми через applySchema()
    // (задача 05, критерии 6/11/12). Под управлением службы рабочий
    // каталог процесса не совпадает с каталогом сборки, поэтому
    // относительный литерал "database" внутри бинарника не находит файлы —
    // путь обязан приходить из конфигурации.
    std::string schemaDir;
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

// Нижняя граница server.max_message_size (docs/task4/02-network-protocol.md,
// раздел 4.1): loadConfig отвергает значение ниже неё как ConfigError.
// Вынесена в заголовок ради тестов границы — так тест сверяется с тем же
// значением, что реально проверяет loadConfig, а не с продублированным
// вручную числом, которое могло бы разойтись с ним при следующей правке.
std::size_t minMaxMessageSize();

}
