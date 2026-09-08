#pragma once

#include <optional>
#include <string>

#include "command_parser.hpp"
#include "command_processor.hpp"
#include "pg_connection.hpp"

namespace matching_engine {

// Слой между Session и обработкой конкретной команды (REQ-NET-11, план
// ДЗ-4 раздел 1.6): Session не знает, как устроена книга заявок, — она
// передаёт сюда уже отделённую кодеком полезную нагрузку одного кадра и
// получает назад готовую строку JSON-ответа. Ни о сокетах, ни об Asio,
// ни о формате кадра этот класс не знает — с его точки зрения запрос уже
// стал обычной строкой.
//
// PING маршрутизируется до разбора команды предметной области — служебная
// команда транспортного уровня, у неё нет command_id, она не идёт в БД и
// не касается книги (docs/task4/02-network-protocol.md, раздел 2). PRINT
// тоже не идёт в CommandProcessor: читает книгу напрямую через
// processor_.orderBook(), как и в консольном режиме — у него тоже нет
// command_id и нечего сохранять. ADD/CANCEL/MODIFY идут через тот же
// CommandParser и CommandProcessor, что и в консольном режиме
// (REQ-API-01/02/03).
// Результат маршрутизации одного запроса: готовый JSON-ответ и признак
// фатального завершения. Возвращается по значению вместо out-параметра
// bool& — у структуры нет перегрузки, которая могла бы этот признак молча
// потерять: перегрузка handle(payload) без fatal была бы самым простым
// способом случайно пропустить остановку сервиса, которую требует раздел
// 3.5 контракта после сбоя сохранения в БД.
struct RouteResult {
    std::string payload;

    // true ровно тогда, когда сохранение результата в БД не удалось
    // (PersistenceError): книга в памяти уже разошлась с хранилищем,
    // payload уже содержит INTERNAL_ERROR, но после его отправки Session
    // обязана закрыть соединение, а сервис — завершить работу (раздел 3.5,
    // последний абзац). При любом другом исходе остаётся false.
    bool fatal = false;

    // Тот же command_id, что уже зашит в payload (эхо или его отсутствие).
    // Нужен Session отдельно от payload: если payload сам не помещается в
    // max_message_size, Session строит новый, короткий ответ
    // RESPONSE_TOO_LARGE и обязана эхировать в нём тот же command_id
    // (docs/task4/02-network-protocol.md, раздел 3.5) — не разбирая payload
    // обратно в JSON ради одного поля.
    std::optional<std::string> commandId;
};

class RequestRouter {
public:
    // Предел длины command_id вынесен в ResponseSerializer::kMaxCommandIdLength
    // (он ограничивает форму ответа, а не путь маршрутизации) — здесь только
    // используется при проверке длины входящего command_id.

    // processor нужен всегда: и для PRINT (только чтение книги), и для
    // изменяющих команд. connection нужен только изменяющим командам —
    // CommandProcessor::process пишет в БД в транзакции. Умолчания у
    // connection нет специально: вырожденный router без соединения даёт
    // сервер, который на каждую изменяющую команду молча отвечает
    // INTERNAL_ERROR, ничего не логируя, — вызывающая сторона обязана
    // передать nullptr явно, если действительно так и задумано (только в
    // тестах формата кадра). Сервер (server_main.cpp) всегда передаёт живое
    // соединение.
    RequestRouter(CommandProcessor& processor, PgConnection* connection);

    // Разбирает payload как JSON и маршрутизирует по полю "type". Ошибка
    // пользователя (не JSON, payload не объект, поле "type" отсутствует
    // или не строка, неизвестный тип, ошибка предметной области) не
    // бросает исключение — превращается в обычный ответ
    // {"status":"ERROR","error":"...","message":"..."}
    // (docs/task4/02-network-protocol.md, раздел 3.5). Соединение эта
    // ошибка не закрывает — решение об этом принимает Session.
    RouteResult handle(const std::string& payload) const;

private:
    RouteResult handleDomainCommand(const nlohmann::json& request) const;

    CommandParser parser_;
    CommandProcessor& processor_;
    PgConnection* connection_;
};

}
