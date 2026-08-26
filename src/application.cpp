#include<nlohmann/json.hpp>
#include<iostream>
#include"application.hpp"
#include"logger.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"database_config.hpp"

namespace matching_engine{

namespace{
constexpr const char* kDefaultConfigPath = "config/database.json";
constexpr const char* kUsage = "Usage: matching_engine [--config <path>] '<json>'\n";
}

bool Application::parseArgs(int argc, char** argv, std::string& configPath,
    std::string& jsonArg, std::string& errorMessage){
    configPath = kDefaultConfigPath;
    bool hasJsonArg = false;
    errorMessage.clear();

    for(int i = 1; i < argc; ++i){
        const std::string arg = argv[i];
        if(arg == "--config"){
            if(i + 1 >= argc){
                errorMessage = "Option --config requires a path argument";
                return false;
            }
            configPath = argv[++i];
        } else if(arg.rfind("--", 0) == 0){
            errorMessage = "Unknown option: " + arg;
            return false;
        } else if(hasJsonArg){
            errorMessage = "Unexpected extra argument: " + arg;
            return false;
        } else {
            jsonArg = arg;
            hasJsonArg = true;
        }
    }

    if(!hasJsonArg){
        errorMessage = kUsage;
        return false;
    }

    return true;
}

int Application::run(int argc, char** argv){
    Logger::instance().info("Application started");

    std::string configPath;
    std::string jsonArg;
    std::string parseError;
    if(!parseArgs(argc, argv, configPath, jsonArg, parseError)){
        if(parseError == kUsage){
            std::cerr << parseError;
        } else {
            printer_.printError(parseError);
        }
        return 1;
    }

    try{
        DatabaseConfig config = loadDatabaseConfig(configPath);
        // Проверка доступности БД при старте: соединение открывается и сразу
        // закрывается, само оно нигде не используется. Нужна, чтобы неверные
        // параметры подключения обнаруживались до обработки команд.
        PgConnection startupProbe(config.host, config.port, config.dbname,
            config.user, config.password);
        Logger::instance().info("Connected to database");
    } catch(const ConfigError& e){
        Logger::instance().error(std::string("Config error: ") + e.what());
        printer_.printError(e.what());
        return 1;
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Database connection error: ") + e.what());
        printer_.printError(e.what());
        return 1;
    }

    nlohmann::json root;
    try{
        root = nlohmann::json::parse(jsonArg);
    } catch(const nlohmann::json::parse_error& e){
        Logger::instance().error(std::string("JSON parse error: ") + e.what());
        printer_.printError(std::string("Invalid JSON: ") + e.what());
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