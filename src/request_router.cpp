#include "request_router.hpp"

#include <nlohmann/json.hpp>

#include "command.hpp"
#include "exceptions.hpp"
#include "logger.hpp"
#include "response_serializer.hpp"

namespace matching_engine {

namespace {

// type, в отличие от command_id, — не идентификатор, а часть текста для
// человека в message, поэтому усечение с многоточием допустимо.
constexpr std::size_t kMaxTypeInMessageLength = 64;

// Реальные command_id на порядок короче этого предела; неправдоподобно
// длинный отвергается как ошибка запроса ещё до разбора и выполнения
// команды (docs/task4/02-network-protocol.md, разделы 3.1 и 3.5) — иначе
// для любого max_message_size нашёлся бы допустимый запрос, ответ на
// который лимит превысит, а при успешном выполнении команда оказалась бы
// выполнена и записана в базу, но клиент не получил бы command_id, по
// которому сопоставляет ответ со своим запросом.
constexpr std::size_t kMaxCommandIdLength = 128;

std::string truncateForMessage(const std::string& value, std::size_t maxLength) {
    if (value.size() <= maxLength) {
        return value;
    }
    return value.substr(0, maxLength) + "...";
}

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

}

RequestRouter::RequestRouter(CommandProcessor& processor, PgConnection* connection)
    : processor_(processor), connection_(connection) {}

RouteResult RequestRouter::handleDomainCommand(const nlohmann::json& request) const {
    std::unique_ptr<Command> command;
    try {
        command = parser_.parse(request);
    } catch (const InvalidOrderValueError& e) {
        // Значение не проходит проверку предметной области (неположительная
        // цена/количество, неизвестная сторона) — более специфичный код
        // таблицы раздела 3.5, чем общий INVALID_REQUEST ниже. Ловится
        // раньше базового ParseError/MatchingEngineError по правилам
        // перегрузки catch.
        return {ResponseSerializer::error(extractCommandId(request), "INVALID_ORDER", e.what()),
            false};
    } catch (const MatchingEngineError& e) {
        return {ResponseSerializer::error(extractCommandId(request), "INVALID_REQUEST", e.what()),
            false};
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
            "Database connection is not configured"), false};
    }

    try {
        const ExecutionResult result = processor_.process(*command, *connection_);
        return {ResponseSerializer::success(commandId, orderId, result), false};
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
            "Failed to save results to the database"), true};
    } catch (const DuplicateOrderError& e) {
        return {ResponseSerializer::error(commandId, "DUPLICATE_ORDER", e.what()), false};
    } catch (const OrderBookError& e) {
        return {ResponseSerializer::error(commandId, "ORDER_NOT_FOUND", e.what()), false};
    } catch (const MatchingEngineError& e) {
        // Остаток иерархии (в первую очередь ParseError: command_id
        // отсутствует) — ближайший код таблицы раздела 3.5 для всего, что
        // не подошло под более специфичные ветки выше.
        return {ResponseSerializer::error(commandId, "INVALID_REQUEST", e.what()), false};
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
            "Request payload is not valid JSON"), false};
    }

    if (!request.is_object() || !request.contains("type") || !request["type"].is_string()) {
        return {ResponseSerializer::error(extractCommandId(request), "INVALID_REQUEST",
            "Request must be a JSON object with a string \"type\" field"), false};
    }

    // Длина command_id проверяется здесь, для любого типа запроса и до
    // всякой попытки его выполнить (docs/task4/02-network-protocol.md,
    // раздел 3.1): для изменяющих команд это единственный способ не
    // оказаться в ситуации "команда выполнена и записана в базу, а эхо в
    // ответе потеряно". Сам неправдоподобно длинный командный id не
    // эхируется — эхировать в ответе об ошибке нечего.
    if (request.contains("command_id") && request["command_id"].is_string() &&
        request["command_id"].get<std::string>().size() > kMaxCommandIdLength) {
        return {ResponseSerializer::error(std::nullopt, "INVALID_REQUEST",
            "command_id is implausibly long"), false};
    }

    const std::string type = request["type"].get<std::string>();
    if (type == "PING") {
        return {buildPong(), false};
    }

    if (type == "PRINT") {
        return {ResponseSerializer::printBook(processor_.orderBook(), extractCommandId(request)),
            false};
    }

    if (type == "ADD" || type == "CANCEL" || type == "MODIFY") {
        return handleDomainCommand(request);
    }

    // HEALTH подключается в задаче 10 (docs/task4/02-network-protocol.md,
    // раздел 2: "PING и HEALTH не проходят через разбор команд предметной
    // области"); любой другой тип, включая HEALTH, получает тот же ответ,
    // что и по-настоящему неизвестная команда. type в тексте message
    // усекается: это пояснение для человека, а не идентификатор, который
    // клиенту нужно сопоставить с запросом.
    return {ResponseSerializer::error(extractCommandId(request), "INVALID_REQUEST",
        "Unknown command type: " + truncateForMessage(type, kMaxTypeInMessageLength)), false};
}

}
