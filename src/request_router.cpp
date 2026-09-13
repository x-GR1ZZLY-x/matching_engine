#include "request_router.hpp"

#include <nlohmann/json.hpp>

#include "command.hpp"
#include "exceptions.hpp"
#include "logger.hpp"
#include "response_serializer.hpp"

namespace matching_engine {

namespace {

// type, в отличие от command_id, — не идентификатор, а часть текста для
// человека в message, поэтому усечение с многоточием допустимо. Усечение —
// ResponseSerializer::truncateUtf8, общая с error(): substr() по числу байт
// не знает о границах символов и на многобайтовом UTF-8 может оставить
// невалидный хвост, на котором nlohmann::json::dump() бросает исключение.
constexpr std::size_t kMaxTypeInMessageLength = 64;

// command_id извлекается прямо из сырого JSON запроса, а не из разобранной
// команды: он должен попасть в ответ и тогда, когда сам разбор команды
// провалился (например, отсутствует обязательное поле "price") — раздел
// 3.5 контракта требует эха, если "запрос удалось разобрать настолько,
// чтобы его извлечь", а не полного успешного разбора команды.
std::optional<std::string> extractCommandId(const nlohmann::json& request) {
    if (request.is_object() && request.contains("command_id") &&
        request["command_id"].is_string()) {
        return request["command_id"].get<std::string>();
    }
    return std::nullopt;
}

// AddCommand и MarketAddCommand оба несут CommandType::Add ("Command: тип
// команды не однозначно определяет класс") — та же развилка, что и в
// MatchingEngine::process, здесь нужна не для выбора ветки сопоставления,
// а чтобы прочитать id_ из конкретного подкласса Command. PrintCommand
// сюда никогда не попадает: PRINT обрабатывается отдельной веткой
// RequestRouter::handle, минуя handleDomainCommand. Бросает тип из
// иерархии проекта (а не std::logic_error): дорога сюда сегодня
// недостижима (все ветки handleDomainCommand строят только эти четыре типа
// команд), но случись это — вызов стоит до try/catch этой функции (см.
// handleDomainCommand), исключение не ловится здесь и уходит выше, в
// Session, которая оборачивает вызов RequestRouter::handle в свой try и
// закрывает только это соединение, не роняя сервис (REQ-API-07).
int extractOrderId(const Command& command) {
    if (const auto* add = dynamic_cast<const AddCommand*>(&command)) {
        return add->id_;
    }
    if (const auto* marketAdd = dynamic_cast<const MarketAddCommand*>(&command)) {
        return marketAdd->id_;
    }
    if (const auto* cancel = dynamic_cast<const CancelCommand*>(&command)) {
        return cancel->id_;
    }
    if (const auto* modify = dynamic_cast<const ModifyCommand*>(&command)) {
        return modify->id_;
    }
    throw MatchingEngineError("extractOrderId: command has no order id");
}

// Формат зафиксирован буквально требованием (REQ-API-04) и не
// расширяется.
std::string buildPong() {
    nlohmann::json response;
    response["status"] = "OK";
    response["result"] = "PONG";
    return response.dump();
}

// Формат зафиксирован буквально требованием (REQ-EXT-06) и не расширяется —
// как и у PING. connection равен nullptr в вырожденной конфигурации без БД
// (только тесты формата кадра, docs/task4/02-network-protocol.md, раздел
// 3.3): книги это не касается, но подключения к базе в такой конфигурации
// точно нет, поэтому "database" честно принимает значение, отличное от
// "CONNECTED", а не литерал, скрывающий отсутствие проверки. Значение того
// же поля на живом соединении вычисляется PgConnection::isConnected()
// (PQstatus), а не констатируется по факту ненулевого указателя: указатель
// мог пережить сам разрыв соединения на стороне сервера БД. isConnected()
// отражает состояние, известное libpq по итогам последней выполненной
// команды, а не результат опроса сервера прямо сейчас — молчаливо оборванное
// соединение (сервер умер, а команд с тех пор не было) обнаруживается только
// на следующем запросе.
std::string buildHealth(const PgConnection* connection) {
    nlohmann::json response;
    response["status"] = "OK";
    response["database"] = (connection != nullptr && connection->isConnected())
        ? "CONNECTED" : "DOWN";
    response["engine"] = "READY";
    return response.dump();
}

}

RequestRouter::RequestRouter(CommandProcessor& processor, PgConnection* connection)
    : processor_(processor), connection_(connection) {}

RouteResult RequestRouter::handleDomainCommand(const nlohmann::json& request) const {
    // Вычисляется один раз и переиспользуется в обеих ветках catch ниже:
    // запрос не меняется между ними, а extractCommandId делает собственный
    // обход JSON и копию строки на каждый вызов (замечание ревью, п.6).
    const std::optional<std::string> commandIdFromRequest = extractCommandId(request);
    std::unique_ptr<Command> command;
    try {
        command = parser_.parse(request);
    } catch (const InvalidOrderValueError& e) {
        // Значение не проходит проверку предметной области (неположительная
        // цена/количество, неизвестная сторона) — более специфичный код
        // таблицы раздела 3.5, чем общий INVALID_REQUEST ниже. Ловится
        // раньше базового ParseError/MatchingEngineError по правилам
        // перегрузки catch.
        return {ResponseSerializer::error(commandIdFromRequest, "INVALID_ORDER", e.what()),
            false, commandIdFromRequest};
    } catch (const MatchingEngineError& e) {
        return {ResponseSerializer::error(commandIdFromRequest, "INVALID_REQUEST", e.what()),
            false, commandIdFromRequest};
    }

    const std::optional<std::string>& commandId = command->commandId_;

    // Идентификатор заявки вычисляется до вызова processor_.process(): если
    // бы extractOrderId бросил (сегодня недостижимо), команда не должна
    // считаться выполненной и закоммиченной в базу без ответа с её
    // результатом.
    const int orderId = extractOrderId(*command);

    if (connection_ == nullptr) {
        // Вырожденная конфигурация без подключения к БД (только тесты
        // формата кадра, где ADD/CANCEL/MODIFY не отправляются): книга в
        // памяти не расходится с хранилищем, поэтому fatal не взводится —
        // это не тот сбой сохранения, о котором говорит раздел 3.5.
        // server_main.cpp всегда передаёт живое соединение; попадание сюда
        // означает ошибку конфигурации вызывающей стороны, а не штатный
        // путь, поэтому оно попадает в лог.
        Logger::instance().error(
            "RequestRouter has no database connection: state-changing command rejected");
        return {ResponseSerializer::error(commandId, "INTERNAL_ERROR",
            "Database connection is not configured"), false, commandId};
    }

    try {
        const ExecutionResult result = processor_.process(*command, *connection_);
        RouteResult route{
            ResponseSerializer::success(commandId, orderId, result), false, commandId};
        // Нужны Session на случай, если сам этот payload не поместится в
        // max_message_size (docs/task4/02-network-protocol.md, раздел 4.1,
        // "Изменяющая команда: команда выполнена, ответ не доставлен") —
        // команда к этому моменту уже выполнена и сохранена в БД.
        route.orderId = orderId;
        route.tradesCount = result.trades.size();
        return route;
    } catch (const PersistenceError& e) {
        // Книга в памяти уже изменена (движок отработал до броска
        // исключения), а запись в БД не удалась — продолжать обслуживание
        // нельзя (docs/task4/02-network-protocol.md, раздел 3.5). Ответ
        // клиенту уходит как обычно, а fatal сообщает Session и дальше
        // Server/main, что после отправки этого ответа нужно закрыть
        // соединение и завершить процесс с кодом 1.
        Logger::instance().error(
            std::string("Persistence error, shutting down: ") + e.what());
        return {ResponseSerializer::error(commandId, "INTERNAL_ERROR",
            "Failed to save results to the database"), true, commandId};
    } catch (const DuplicateOrderError& e) {
        return {ResponseSerializer::error(commandId, "DUPLICATE_ORDER", e.what()), false,
            commandId};
    } catch (const OrderBookError& e) {
        return {ResponseSerializer::error(commandId, "ORDER_NOT_FOUND", e.what()), false,
            commandId};
    } catch (const MatchingEngineError& e) {
        // Остаток иерархии (в первую очередь ParseError: command_id
        // отсутствует) — ближайший код таблицы раздела 3.5 для всего, что
        // не подошло под более специфичные ветки выше.
        return {ResponseSerializer::error(commandId, "INVALID_REQUEST", e.what()), false,
            commandId};
    }
}

RouteResult RequestRouter::handle(const std::string& payload) const {
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error&) {
        // Пустая полезная нагрузка (docs/task4/02-network-protocol.md,
        // раздел 1.2) тоже проваливает разбор JSON и приходит сюда же —
        // отдельной ветки для неё не нужно.
        return {ResponseSerializer::error(std::nullopt, "INVALID_REQUEST",
            "Request payload is not valid JSON"), false, std::nullopt};
    }

    // Длина command_id проверяется здесь, раньше всех прочих проверок
    // запроса — в частности, раньше проверки поля "type" ниже — и до всякой
    // попытки его выполнить (docs/task4/02-network-protocol.md, раздел 3.1):
    // фильтр не зависит от того, есть ли в запросе "type", и обязан
    // применяться к любому JSON-значению, разобравшемуся из полезной
    // нагрузки. До этой правки проверка стояла после проверки "type", и
    // запрос без "type" уходил в ответ с неотфильтрованным эхом, которое
    // само могло превысить max_message_size (замечание ревью второго
    // круга, п.1) — enqueueResponse в Session бросал, а RESPONSE_TOO_LARGE
    // строился с тем же неотфильтрованным эхом и бросал снова, разрывая
    // соединение без единого байта ответа. contains() возвращает false для
    // необъектных значений (bool/array/...), поэтому явная проверка
    // is_object() здесь не нужна. Для изменяющих команд это единственный
    // способ не оказаться в ситуации "команда выполнена и записана в базу,
    // а эхо в ответе потеряно". Сам неправдоподобно длинный командный id не
    // эхируется — эхировать в ответе об ошибке нечего.
    if (request.contains("command_id") && request["command_id"].is_string() &&
        request["command_id"].get<std::string>().size() > ResponseSerializer::kMaxCommandIdLength) {
        return {ResponseSerializer::error(std::nullopt, "INVALID_REQUEST",
            "command_id is implausibly long"), false, std::nullopt};
    }

    if (!request.is_object() || !request.contains("type") || !request["type"].is_string()) {
        const std::optional<std::string> commandId = extractCommandId(request);
        return {ResponseSerializer::error(commandId, "INVALID_REQUEST",
            "Request must be a JSON object with a string \"type\" field"), false, commandId};
    }

    const std::string type = request["type"].get<std::string>();
    if (type == "PING") {
        return {buildPong(), false, std::nullopt};
    }

    if (type == "HEALTH") {
        // Служебная команда транспортного уровня, как и PING (docs/task4/
        // 02-network-protocol.md, раздел 2): маршрутизируется до разбора
        // команды предметной области, у неё нет command_id, она не идёт в
        // БД и не касается книги (REQ-EXT-07) — connection_ читается, а не
        // изменяется.
        return {buildHealth(connection_), false, std::nullopt};
    }

    if (type == "PRINT") {
        const std::optional<std::string> commandId = extractCommandId(request);
        return {ResponseSerializer::printBook(processor_.orderBook(), commandId), false,
            commandId};
    }

    if (type == "ADD" || type == "CANCEL" || type == "MODIFY") {
        return handleDomainCommand(request);
    }

    // Любой другой тип — по-настоящему неизвестная команда. type в тексте
    // message усекается: это пояснение для человека, а не идентификатор,
    // который клиенту нужно сопоставить с запросом.
    const std::optional<std::string> commandId = extractCommandId(request);
    return {ResponseSerializer::error(commandId, "INVALID_REQUEST",
        "Unknown command type: " +
            ResponseSerializer::truncateUtf8(type, kMaxTypeInMessageLength)),
        false, commandId};
}

}
