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
constexpr const char* kUsage =
    "Usage: matching_engine [--config <path>] '<json>'\n"
    "  Commands that change state (ADD, CANCEL, MODIFY) must include a "
    "unique \"command_id\" field; PRINT does not need one.\n";

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
    // при выходе из функции — ручного закрытия соединения нет нигде в этом
    // файле (REQ-RAII-09, критерий 8 задачи 07).
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

    // root["commands"] бросает nlohmann::json::type_error, если корень —
    // не объект (например, top-level число или массив); это исключение не
    // наследует MatchingEngineError и ничем в этом файле не перехватывается,
    // поэтому проверка нужна раньше, до входа в try ниже, иначе это тот же
    // неперехваченный terminate, от которого предостерегает критерий 5.
    if(!root.is_object() || !root.contains("commands") || !root["commands"].is_array()){
        const std::string message = "Invalid input: top-level JSON must be an object "
            "with a \"commands\" array field.";
        Logger::instance().error(message);
        printer_.printError(message);
        return 1;
    }

    // Сбой сохранения — фатальная, а не командная ошибка (docs/plan.md,
    // "Обработка ошибок: два разных класса"): к моменту, когда
    // PersistenceError долетает сюда, книга в памяти уже изменена, а в БД —
    // нет, продолжать работу на расходящемся состоянии нельзя. Обработчик
    // здесь, вокруг всего цикла, — это то самое место, о котором
    // предупреждает критерий 5: без него исключение размотало бы стек мимо
    // main() и обернулось бы неперехваченным std::terminate.
    try{
        for(const auto& commandJson : root["commands"]){
            processCommand(commandJson, *connection);
        }
    } catch(const PersistenceError& e){
        Logger::instance().error(std::string("Persistence error: ") + e.what());
        printer_.printError(std::string("Failed to save results to the database, "
            "aborting: ") + e.what());
        return 1;
    }

    Logger::instance().info("Application finished");
    return 0;
}

void Application::processCommand(const nlohmann::json& commandJson, PgConnection& connection){
    std::unique_ptr<Command> command;

    try{
        command = parser_.parse(commandJson);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Parse error: ") + e.what());
        printer_.printError(e.what());
        return;
    }

    if(command->type_ == CommandType::Print){
        printer_.printOrderBook(processor_.orderBook());
        return;
    }

    ExecutionResult result;
    try{
        result = processor_.process(*command, connection);
    } catch(const PersistenceError&){
        // PersistenceError наследует MatchingEngineError (конвенция
        // проекта) — обязан быть перехвачен и проброшен раньше
        // catch(const MatchingEngineError&) ниже, иначе тот перехватит его
        // как обычную командную ошибку и проглотит (docs/tasks/task-07.md,
        // п.5). Дальше исключение ловит цикл в run().
        throw;
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Engine error: ") + e.what());
        printer_.printError(e.what());
        return;
    }

    for(const auto& trade : result.trades){
        printer_.printTrade(trade);
    }
}

}