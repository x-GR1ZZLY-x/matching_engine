#include<nlohmann/json.hpp>
#include<iostream>
#include"application.hpp"
#include"logger.hpp"
#include"exceptions.hpp"

namespace matching_engine{

int Application::run(int argc, char** argv){
    Logger::instance().info("Application started");

    if(argc < 2){
        std::cerr << "Usage: matching_engine '<json>'\n";
        return 1;
    }

    nlohmann::json root;
    try{
        root = nlohmann::json::parse(argv[1]);
    } catch(const nlohmann::json::parse_error& e){
        Logger::instance().error(std::string("JSON parse error: ") + e.what());
        printer_.printError(std::string("Invflid JSON: ") + e.what());
        return 1;
    }

    for(const auto& commandJson : root["commands"]){
        processCommand(commandJson);
    }

    Logger::instance().info("Application finished");
    return 0;
}

void Application::processCommand(const nlohmann::json& commandJson){
    std::unique_ptr<Command> command;

    try{
        command = parser_.parse(commandJson);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Parse error: ") + e.what());
        printer_.printError(e.what());
        return;
    }
    
    if(command->type_ == CommandType::Print){
        printer_.printOrderBook(engine_.orderBook());
        return;
    }

    const size_t tradesBefore = engine_.trades().size();

    try{
        engine_.process(*command);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Engine error: ") + e.what());
        printer_.printError(e.what());
        return;
    }

    const auto& allTrades = engine_.trades();
    for(size_t i = tradesBefore; i < allTrades.size(); ++i){
        printer_.printTrade(allTrades[i]);
    }
}

}