#pragma once

#include <string>

namespace matching_engine{

// Разбор argv для matching-engine-server (REQ-NET-12, частично).
// Единственный ключ — "--config <путь>"; при его отсутствии подставляется
// путь по умолчанию. Возвращает false, если ключ указан без значения либо
// встречен неизвестный аргумент — тогда errorMessage содержит текст ошибки.
bool parseServerArgs(int argc, char** argv, std::string& configPath,
    std::string& errorMessage);

// Разбор argv для matching-engine-client. Ключи "--host <адрес>" и
// "--port <номер>" обязательны оба. Возвращает false при отсутствующем
// значении, неизвестном аргументе, отсутствии обязательного ключа либо
// нечисловом или выходящем за диапазон TCP-портов (0..65535) значении
// "--port" — тогда errorMessage содержит текст ошибки.
bool parseClientArgs(int argc, char** argv, std::string& host, std::string& port,
    std::string& errorMessage);

}
