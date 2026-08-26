#pragma once
#include<string>
#include"command_parser.hpp"
#include"matching_engine.hpp"
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
    MatchingEngine engine_;
    ReportPrinter printer_;

    void processCommand(const nlohmann::json& commandJson);
};

}