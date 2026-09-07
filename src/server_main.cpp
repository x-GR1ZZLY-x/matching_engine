#include<optional>
#include<string>
#include<boost/asio.hpp>
#include"cli_args.hpp"
#include"command_processor.hpp"
#include"config.hpp"
#include"exceptions.hpp"
#include"logger.hpp"
#include"pg_connection.hpp"
#include"recovery_service.hpp"
#include"request_router.hpp"
#include"schema.hpp"
#include"server.hpp"

// Серверный бинарник (REQ-NET-12): поднимает TCP-сервис и обслуживает
// соединения до тех пор, пока io_context не остановится сам. Сигнальный
// поток и graceful shutdown по SIGTERM/SIGINT появляются в задаче 08 —
// сейчас у процесса ещё нет собственного штатного способа завершиться,
// кроме внешнего сигнала операционной системы, который здесь не
// перехватывается.
//
// В серверном бинарнике std::cout не используется вообще: результат
// работы уходит клиенту в сокет, а оба стандартных потока отданы под
// диагностику — она идёт через Logger (stderr), который systemd соберёт
// в journal.
int main(int argc, char** argv){
    using namespace matching_engine;

    std::string configPath;
    std::string errorMessage;
    if(!parseServerArgs(argc, argv, configPath, errorMessage)){
        Logger::instance().error(errorMessage);
        return 1;
    }

    AppConfig config;
    try{
        config = loadConfig(configPath);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Config error: ") + e.what());
        return 1;
    }

    // Порядок запуска обратному не подлежит (REQ-NET-14): подключение к
    // PostgreSQL -> применение схемы -> восстановление книги заявок ->
    // прогрев кеша идемпотентности -> и только затем начало прослушивания
    // порта. Клиент, подключившийся к порту, обязан попасть на уже готовый
    // движок, а не на процесс, который ещё поднимается.
    std::optional<PgConnection> connection;
    try{
        connection.emplace(config.database.host, config.database.port,
            config.database.dbname, config.database.user, config.database.password);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Cannot connect to the database: ") + e.what());
        return 1;
    }
    Logger::instance().info("Connected to database");

    try{
        applySchema(*connection, config.database.schemaDir);
        Logger::instance().info("Database schema applied");
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Schema error: ") + e.what());
        return 1;
    }

    // recoverState (src/recovery_service.cpp, переиспользуется как есть)
    // сама логирует и число восстановленных заявок, и размер прогретого
    // кеша идемпотентности.
    CommandProcessor processor;
    try{
        recoverState(*connection, processor);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Recovery error: ") + e.what());
        return 1;
    }

    // RequestRouter владеет ссылками на processor и connection всё время
    // работы сервера (задача 05): ADD/CANCEL/MODIFY идут через
    // CommandParser и processor, PRINT читает processor.orderBook()
    // напрямую, PING обрабатывается до разбора команды.
    RequestRouter router(processor, &*connection);

    boost::asio::io_context ioContext;

    // Server(...), start() и ioContext.run() бросают исключения не только
    // из MatchingEngineError: make_address/open/bind/listen (например, при
    // занятом порте — самый частый сценарий неудачного запуска) бросают
    // boost::system::system_error, который ничем из перечисленного выше не
    // перехватывается и без этого try/catch дошёл бы до std::terminate.
    bool fatalError = false;
    try {
        Server server(ioContext, config.server, router);

        // start() — последний шаг цепочки: сообщает о готовности строкой
        // "Listening on <адрес>:<порт>" и только после этого начинает
        // принимать соединения.
        server.start();

        ioContext.run();
        fatalError = server.hadFatalError();
    } catch (const std::exception& e) {
        Logger::instance().error(std::string("Server error: ") + e.what());
        return 1;
    }

    // Сбой сохранения в БД (PersistenceError) уже отвечен клиенту как
    // INTERNAL_ERROR и не бросил исключения наружу — io_context.run()
    // вернулся штатно, потому что Server сам остановил его после закрытия
    // виновной сессии (docs/task4/02-network-protocol.md, раздел 3.5).
    // Код возврата 1 при этом существен для systemd: под Restart=on-failure
    // он означает "перезапустить", а не "сервис завершился по плану".
    if (fatalError) {
        Logger::instance().error("Shutting down after a database persistence failure");
        return 1;
    }

    return 0;
}
