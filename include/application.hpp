#pragma once
#include<string>
#include"command_parser.hpp"
#include"command_processor.hpp"
#include"pg_connection.hpp"
#include"report_printer.hpp"

namespace matching_engine{

class Application{
public:
    int run(int argc, char** argv);

    // Разбирает argv: возвращает путь к конфигу (--config или значение по
    // умолчанию) и JSON-пакет команд. Возвращает false, если обработку
    // нужно прервать — тогда errorMessage всегда содержит текст для вывода
    // (справку по использованию либо описание конкретной ошибки разбора).
    static bool parseArgs(int argc, char** argv, std::string& configPath,
        std::string& jsonArg, std::string& errorMessage);

private:
    CommandParser parser_;
    CommandProcessor processor_;
    ReportPrinter printer_;

    // connection передаётся параметром, а не хранится полем Application —
    // соединение живёт в run() (см. её тело) и передаётся по ссылке на
    // каждый вызов, как и в репозиториях/CommandProcessor.
    void processCommand(const nlohmann::json& commandJson, PgConnection& connection);
};

}