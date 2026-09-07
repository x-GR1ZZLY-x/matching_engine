#include "request_router.hpp"

#include <nlohmann/json.hpp>

namespace matching_engine {

namespace {

// Ответ об ошибке обязан оставаться маленьким независимо от того, что
// прислал клиент: обвязка ответа весит больше обвязки запроса, поэтому
// неограниченное эхо пользовательских данных (command_id, type) означает,
// что для любого max_message_size находится допустимый по размеру запрос,
// ответ на который лимит превышает, — и MessageCodec::encode бросает
// MessageTooLargeError вместо того, чтобы отдать INVALID_REQUEST (второй
// круг ревью задачи 04).
//
// command_id длиннее предела в ответ не попадает вовсе, а не усекается:
// усечённый идентификатор клиент не сопоставит со своим запросом, а раздел
// 3.5 контракта говорит о возврате значения, а не его части. Реальные
// command_id на порядок короче этого предела.
constexpr std::size_t kMaxEchoedCommandIdLength = 128;

// type, в отличие от command_id, — не идентификатор, а часть текста для
// человека в message, поэтому усечение с многоточием допустимо.
constexpr std::size_t kMaxTypeInMessageLength = 64;

std::string truncateForMessage(const std::string& value, std::size_t maxLength) {
    if (value.size() <= maxLength) {
        return value;
    }
    return value.substr(0, maxLength) + "...";
}

// Формат ответа-ошибки зафиксирован буквально (docs/task4/
// 02-network-protocol.md, раздел 3.5): status/error/message лежат на
// верхнем уровне, вложенного объекта ошибки нет. command_id возвращается
// эхом, если запрос разобрался в объект и содержит его строковым значением
// не длиннее kMaxEchoedCommandIdLength (раздел 3.5: "command_id
// присутствует, если он был в запросе и запрос удалось разобрать настолько,
// чтобы его извлечь") — request по умолчанию пуст (не объект), поэтому
// вызовы без него ведут себя как раньше.
std::string buildInvalidRequest(const std::string& message,
    const nlohmann::json& request = nlohmann::json()) {
    nlohmann::json response;
    if (request.is_object() && request.contains("command_id") &&
        request["command_id"].is_string()) {
        const std::string& commandId = request["command_id"].get_ref<const std::string&>();
        if (commandId.size() <= kMaxEchoedCommandIdLength) {
            response["command_id"] = commandId;
        }
    }
    response["status"] = "ERROR";
    response["error"] = "INVALID_REQUEST";
    response["message"] = message;
    return response.dump();
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

std::string RequestRouter::handle(const std::string& payload) const {
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error&) {
        // Пустая полезная нагрузка (docs/task4/02-network-protocol.md,
        // раздел 1.2) тоже проваливает разбор JSON и приходит сюда же —
        // отдельной ветки для неё не нужно.
        return buildInvalidRequest("Request payload is not valid JSON");
    }

    if (!request.is_object() || !request.contains("type") || !request["type"].is_string()) {
        return buildInvalidRequest(
            "Request must be a JSON object with a string \"type\" field", request);
    }

    const std::string type = request["type"].get<std::string>();
    if (type == "PING") {
        return buildPong();
    }

    // Команды предметной области и HEALTH подключаются в задаче 05; до тех
    // пор любой другой тип получает тот же ответ, что и по-настоящему
    // неизвестная команда (docs/hw4/tasks/task-04.md, "Границы"). type в
    // тексте message усекается: это пояснение для человека, а не
    // идентификатор, который клиенту нужно сопоставить с запросом.
    return buildInvalidRequest(
        "Unknown command type: " + truncateForMessage(type, kMaxTypeInMessageLength), request);
}

}
