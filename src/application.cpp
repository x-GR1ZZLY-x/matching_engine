#include<nlohmann/json.hpp>
#include<iostream>
#include<optional>
#include"application.hpp"
#include"logger.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"database_config.hpp"
#include"schema.hpp"

namespace matching_engine{

namespace{
constexpr const char* kDefaultConfigPath = "config/database.json";
constexpr const char* kSchemaDir = "database";
constexpr const char* kUsage = "Usage: matching_engine [--config <path>] '<json>'\n";

// Человеческое сообщение для случая недоступной БД: куда шло подключение и
// что проверить. Пароль в сообщение не попадает; сырой текст драйвера
// (PQerrorMessage) уходит только в лог на отладочном уровне через e.what().
std::string describeConnectionFailure(const DatabaseConfig& config){
    return "Cannot connect to the database at " + config.host + ":" + config.port +
        " (database '" + config.dbname + "', user '" + config.user + "'). "
        "Check that the PostgreSQL server is running and reachable at that "
        "address, that host/port/dbname/user in the configuration are correct, "
        "and that the MATCHING_ENGINE_DB_PASSWORD environment variable holds "
        "the current password.";
}

// Человеческое сообщение для случая падения применения схемы: какой каталог
// и что проверить. Сырой текст драйвера (например, PQresultErrorMessage при
// ошибке в SQL) уходит только в лог на отладочном уровне через e.what().
std::string describeSchemaFailure(const std::string& schemaDir){
    return "Failed to apply the database schema from '" + schemaDir + "'. "
        "Check that the directory exists, is readable, and contains valid SQL "
        "files; details are in the log.";
}
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

    DatabaseConfig config;
    try{
        config = loadDatabaseConfig(configPath);
    } catch(const ConfigError& e){
        Logger::instance().error(std::string("Config error: ") + e.what());
        printer_.printError(e.what());
        return 1;
    }

    // Соединение открывается один раз здесь и живёт до конца run() —
    // весь жизненный цикл приложения. Освобождается деструктором PgConnection
    // при выходе из функции, никакого ручного PQfinish не требуется.
    std::optional<PgConnection> connection;
    try{
        connection.emplace(config.host, config.port, config.dbname,
            config.user, config.password);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(describeConnectionFailure(config));
        Logger::instance().debug(std::string("Database connection error: ") + e.what());
        printer_.printError(describeConnectionFailure(config));
        return 1;
    }
    Logger::instance().info("Connected to database");

    try{
        applySchema(*connection, kSchemaDir);
        Logger::instance().info("Database schema applied");
    } catch(const MatchingEngineError& e){
        Logger::instance().error(describeSchemaFailure(kSchemaDir));
        Logger::instance().debug(std::string("Schema error: ") + e.what());
        printer_.printError(describeSchemaFailure(kSchemaDir));
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