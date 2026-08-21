#pragma once
#include"command_parser.hpp"
#include"matching_engine.hpp"
#include"report_printer.hpp"

namespace matching_engine{

class Application{
public:
    int run(int argc, char** argv);
private:
    CommandParser parser_;
    MatchingEngine engine_;
    ReportPrinter printer_;

    void processCommand(const nlohmann::json& commandJson);
};

}