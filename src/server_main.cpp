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
#include"signal_handler.hpp"

// Серверный бинарник (REQ-NET-12): поднимает TCP-сервис, обслуживает
// соединения и останавливается штатно по SIGTERM/SIGINT (docs/task4/
// 01-service-lifecycle.md) — главный поток крутит io_context, отдельный
// сигнальный поток (SignalHandler) ждёт сигналы остановки и передаёт запрос
// через post().
//
// В серверном бинарнике std::cout не используется вообще: результат
// работы уходит клиенту в сокет, а оба стандартных потока отданы под
// диагностику — она идёт через Logger (stderr), который systemd соберёт
// в journal.
int main(int argc, char** argv){
    using namespace matching_engine;

    // Первым действием процесса, до разбора аргументов и до создания
    // каких-либо потоков (раздел 2.1, 2.3 документа): SIGTERM, SIGINT и
    // SIGUSR1 блокируются в маске главного потока, и созданный позже
    // сигнальный поток унаследует её. Без этого сигнал, пришедший во время
    // долгого подключения к БД или восстановления книги, выполнил бы
    // действие по умолчанию и убил бы процесс на середине запуска.
    if (!SignalHandler::blockSignals()) {
        // Сообщение об ошибке уже написано в журнал внутри blockSignals();
        // без рабочей маски вся схема остановки не работает — продолжать
        // запуск нет смысла (раздел 4.4 документа).
        return 1;
    }

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
    // работы сервера: ADD/CANCEL/MODIFY идут через
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
    bool forcedShutdown = false;
    // Взводится onFailure сигнального потока (см. SignalHandler ниже) при
    // отказе самого сигнального потока (ожидание сигналов вернуло ошибку,
    // из тела потока вылетело исключение) — без этого признака такой отказ
    // неотличим снаружи от штатной остановки по SIGTERM и main вернул бы 0
    // (REQ-THR-12, раздел 4.4 документа). Обычный bool, не std::atomic:
    // запись происходит в сигнальном потоке, а чтение — ниже, уже после
    // разрушения signalHandler (то есть после join()), и happens-before
    // между записью и чтением обеспечивает присоединение потока — та же
    // схема, что и у hadFatalError()/wasForceStopped().
    bool signalThreadFailed = false;
    try {
        Server server(ioContext, config.server, router);

        // start() — последний шаг цепочки: сообщает о готовности строкой
        // "Listening on <адрес>:<порт>" и только после этого начинает
        // принимать соединения.
        server.start();

        // Сигнальный поток создаётся после Server (раздел 1.3, 5.4
        // документа): промежуток между блокировкой маски (самое начало
        // main) и этим местом занимает подключение к БД, применение схемы,
        // восстановление книги и прогрев кеша — сигнал, пришедший в это
        // время, остаётся в очереди отложенных сигналов процесса и
        // достаётся сигнальному потоку сразу после его запуска (раздел 2.3).
        // onStop делает ровно то же, что документ предписывает сигнальному
        // потоку: единственная операция, которую он выполняет над
        // состоянием сервера, — post() в io_context (Server::stop() сам
        // является этим post()).
        //
        // Порядок объявления существен и здесь: signalHandler объявлен
        // после server, поэтому разрушается первым — сигнальный поток
        // присоединяется раньше, чем начнёт разрушаться Server, а Server, в
        // свою очередь, раньше, чем ioContext, объявленный выше try (раздел
        // 1.3 документа).
        SignalHandler signalHandler(
            [&server] { server.stop(); },
            [&signalThreadFailed] { signalThreadFailed = true; });

        ioContext.run();
        fatalError = server.hadFatalError();
        forcedShutdown = server.wasForceStopped();
        Logger::instance().info("Event loop stopped");
    } catch (const std::exception& e) {
        Logger::instance().error(std::string("Server error: ") + e.what());
        return 1;
    } catch (...) {
        Logger::instance().error("Server error: unknown exception");
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

    // Принудительное завершение по истечении предельного времени остановки
    // тоже даёт ненулевой код (раздел 4.4 документа): это признак дефекта
    // (незавершённая операция, которую drain не закрыл), а не обычной
    // остановки, и оператор обязан увидеть это в состоянии службы.
    if (forcedShutdown) {
        Logger::instance().error("Shutdown deadline exceeded, stopped forcibly");
        return 1;
    }

    // signalThreadFailed читается здесь, а не раньше: к этому месту
    // signalHandler уже разрушен (конец try-блока выше), то есть SIGUSR1
    // послан и join() завершился — присоединение потока и есть
    // happens-before, на который опирается чтение обычного bool, записанного
    // в другом потоке (раздел 4.4 документа, REQ-THR-12).
    if (signalThreadFailed) {
        Logger::instance().error("Signal thread failed, exiting with a non-zero status");
        return 1;
    }

    Logger::instance().info("Server stopped");
    return 0;
}
