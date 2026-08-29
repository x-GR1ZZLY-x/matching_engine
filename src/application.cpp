#include<nlohmann/json.hpp>
#include<chrono>
#include<fstream>
#include<iostream>
#include<optional>
#include"application.hpp"
#include"logger.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"database_config.hpp"
#include"recovery_service.hpp"
#include"schema.hpp"

namespace matching_engine{

namespace{
constexpr const char* kDefaultConfigPath = "config/database.json";
constexpr const char* kSchemaDir = "database";
constexpr const char* kUsage =
    "Usage: matching_engine [--config <path>] '<json>'\n"
    "       matching_engine [--config <path>] --replay <file.jsonl>\n"
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
    std::string& jsonArg, std::string& replayPath, std::string& errorMessage){
    configPath = kDefaultConfigPath;
    jsonArg.clear();
    replayPath.clear();
    bool hasJsonArg = false;
    bool hasReplay = false;
    errorMessage.clear();

    for(int i = 1; i < argc; ++i){
        const std::string arg = argv[i];
        if(arg == "--config"){
            if(i + 1 >= argc){
                errorMessage = "Option --config requires a path argument";
                return false;
            }
            configPath = argv[++i];
        } else if(arg == "--replay"){
            if(i + 1 >= argc){
                errorMessage = "Option --replay requires a path argument";
                return false;
            }
            replayPath = argv[++i];
            hasReplay = true;
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

    if(hasReplay && hasJsonArg){
        errorMessage = "Cannot use --replay together with a JSON argument";
        return false;
    }

    if(!hasReplay && !hasJsonArg){
        errorMessage = kUsage;
        return false;
    }

    return true;
}

int Application::run(int argc, char** argv){
    Logger::instance().info("Application started");

    std::string configPath;
    std::string jsonArg;
    std::string replayPath;
    std::string parseError;
    if(!parseArgs(argc, argv, configPath, jsonArg, replayPath, parseError)){
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

    // Восстановление обязано завершиться прежде первой обрабатываемой
    // команды (docs/tasks/task-08.md): книга, счётчик номеров и кеш
    // идемпотентности должны быть в порядке, унаследованном от прошлых
    // запусков, до того как движок увидит новый JSON-пакет.
    try{
        recoverState(*connection, processor_);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Recovery error: ") + e.what());
        printer_.printError(std::string("Failed to recover state from the database: ") +
            e.what());
        return 1;
    }

    // Режим воспроизведения нагрузки идёт по отдельной ветке, но через ту
    // же цепочку старта выше (конфигурация -> соединение -> схема ->
    // recoverState) — критерий 7 задачи 10. PersistenceError фатальна и в
    // этом режиме (docs/plan.md, "Обработка ошибок: два разных класса"),
    // поэтому она перехватывается здесь так же, как ниже для обычного
    // режима, а не поглощается внутри runReplay.
    if(!replayPath.empty()){
        bool replayOk = false;
        try{
            replayOk = runReplay(replayPath, *connection);
        } catch(const PersistenceError& e){
            Logger::instance().error(std::string("Persistence error: ") + e.what());
            printer_.printError(std::string("Failed to save results to the database, "
                "aborting: ") + e.what());
            return 1;
        }
        if(!replayOk){
            return 1;
        }
        Logger::instance().info("Application finished");
        return 0;
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
            processCommand(commandJson, *connection, /*printTrades=*/true);
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

Application::CommandOutcome Application::processCommand(const nlohmann::json& commandJson,
    PgConnection& connection, bool printTrades){
    std::unique_ptr<Command> command;

    try{
        command = parser_.parse(commandJson);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Parse error: ") + e.what());
        printer_.printError(e.what());
        return CommandOutcome{CommandOutcome::Status::Failed, 0};
    }

    if(command->type_ == CommandType::Print){
        printer_.printOrderBook(processor_.orderBook());
        return CommandOutcome{CommandOutcome::Status::Printed, 0};
    }

    ExecutionResult result;
    bool servedFromCache = false;
    try{
        result = processor_.process(*command, connection, &servedFromCache);
    } catch(const PersistenceError&){
        // PersistenceError наследует MatchingEngineError (конвенция
        // проекта) — обязан быть перехвачен и проброшен раньше
        // catch(const MatchingEngineError&) ниже, иначе тот перехватит его
        // как обычную командную ошибку и проглотит (docs/tasks/task-07.md,
        // п.5). Дальше исключение ловит цикл в run() (обычный режим) либо
        // runReplay (режим воспроизведения) — обоим он должен быть фатален.
        throw;
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Engine error: ") + e.what());
        printer_.printError(e.what());
        return CommandOutcome{CommandOutcome::Status::Failed, 0};
    }

    // В обычном режиме (printTrades == true) каждая сделка печатается сразу,
    // как и раньше, — независимо от того, обслужена ли команда из кеша
    // идемпотентности (поведение обычного режима задачей 10 менять
    // запрещено). В режиме --replay печать отдельных строк TRADE намеренно
    // отключена (docs/tasks/task-10.md): при 100 000+ командах это были бы
    // десятки тысяч строк в stdout, а задача 11 профилирует именно этот
    // режим — вывод в терминал исказил бы замер работы движка и БД. Число
    // сделок вместо этого уходит в итоговую сводку.
    if(printTrades){
        for(const auto& trade : result.trades){
            printer_.printTrade(trade);
        }
    }

    if(servedFromCache){
        // Ревью задачи 10, правка 1: команда пришла из кеша идемпотентности
        // и в этом вызове ничего не записала в БД — trades == 0, чтобы
        // runReplay не считал сделки повтора второй раз.
        return CommandOutcome{CommandOutcome::Status::Duplicate, 0};
    }
    return CommandOutcome{CommandOutcome::Status::Applied,
        static_cast<int>(result.trades.size())};
}

bool Application::runReplay(const std::string& path, PgConnection& connection){
    std::ifstream file(path);
    if(!file.is_open()){
        Logger::instance().error("Cannot open replay file: " + path);
        printer_.printError("Cannot open replay file: " + path);
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    // Счётчики отражают три разных исхода команды, а не долю строк файла
    // (ревью задачи 10, правка 1). Обещание перед критерием 7: processed ==
    // число строк, реально добавленных в processed_commands в этом прогоне;
    // trades == число строк, добавленных в trades. duplicates и skipped в
    // это равенство не входят — строки PRINT тоже не входят ни в один
    // счётчик. Сумма счётчиков поэтому не обязана равняться числу строк
    // файла.
    long long processedCount = 0;
    long long tradeCount = 0;
    long long duplicateCount = 0;
    long long skippedCount = 0;

    // Потоковое чтение построчно (REQ-PERF-02, критерий 4 задачи 10): файл
    // не грузится в память целиком, в любой момент в памяти — одна строка.
    std::string line;
    while(std::getline(file, line)){
        if(line.find_first_not_of(" \t\r\n") == std::string::npos){
            continue; // Пустые и пробельные строки пропускаются молча.
        }

        nlohmann::json commandJson;
        try{
            commandJson = nlohmann::json::parse(line);
        } catch(const nlohmann::json::parse_error& e){
            Logger::instance().error(std::string("Replay: invalid JSON line: ") + e.what());
            printer_.printError(std::string("Invalid JSON line: ") + e.what());
            ++skippedCount;
            continue;
        }

        // PersistenceError не перехватывается здесь — она должна дойти до
        // run(), который завершает процесс (см. критерий 4 задания и
        // docs/plan.md, "Обработка ошибок: два разных класса").
        const CommandOutcome outcome = processCommand(commandJson, connection,
            /*printTrades=*/false);
        switch(outcome.status){
            case CommandOutcome::Status::Applied:
                ++processedCount;
                tradeCount += outcome.trades;
                break;
            case CommandOutcome::Status::Duplicate:
                ++duplicateCount;
                break;
            case CommandOutcome::Status::Printed:
                break;
            case CommandOutcome::Status::Failed:
                ++skippedCount;
                break;
        }
    }

    // getline возвращает false и по достижении конца файла (eof, прогон
    // успешен), и при сбое чтения потока (failbit/badbit без eof — ревью
    // задачи 10, правка 4: путь указывает на каталог, поток открывается,
    // но каждый getline проваливается). Пустой файл — законный вход и не
    // должен приниматься за ошибку, поэтому проверяем именно badbit, а не
    // общий !file.
    if(file.bad()){
        Logger::instance().error("Replay: read error on file: " + path);
        printer_.printError("Failed to read replay file: " + path);
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    printer_.printReplaySummary(processedCount, tradeCount, duplicateCount, skippedCount, elapsed);
    return true;
}

}