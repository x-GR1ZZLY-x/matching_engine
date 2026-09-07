#pragma once

#include <optional>
#include <string>

#include "execution_result.hpp"
#include "order_book.hpp"

namespace matching_engine {

// Строит JSON ответа клиенту (docs/task4/02-network-protocol.md, раздел 3):
// успех изменяющей команды, ошибку и снимок книги для PRINT. Отдельный
// компонент от CommandProcessor::serializeResult/parseResult (план ДЗ-4,
// раздел 1.6): формат ответа клиенту (внешний интерфейс) и формат
// processed_commands.result (хранение для восстановления) описывают разное
// и не должны срастись в одну функцию — иначе либо во внешний ответ утекли
// бы внутренние снимки заявок (order_changes), либо правка API незаметно
// испортила бы формат хранения.
class ResponseSerializer {
public:
    // Успешное выполнение ADD/CANCEL/MODIFY (раздел 3.1). orderId —
    // идентификатор заявки, которой касалась команда; commandId — эхо
    // запроса. Один и тот же результат от CommandProcessor::process,
    // полученный ли свежим выполнением или из кеша идемпотентности,
    // сериализуется этой же функцией и даёт тот же ответ (раздел 3.6) —
    // вызывающая сторона не должна различать эти два случая.
    static std::string success(const std::optional<std::string>& commandId, int orderId,
        const ExecutionResult& result);

    // Ошибка (раздел 3.5): command_id, status, error и message лежат на
    // верхнем уровне, вложенного объекта ошибки нет. commandId
    // отсутствует в ответе, если он не был известен (запрос не разобрался
    // в объект) или неправдоподобно длинный (защита от раздувания ответа
    // эхом входа: иначе для любого max_message_size нашёлся бы запрос,
    // ответ на который лимит превысит); message длиннее разумного предела
    // усекается тем же способом.
    static std::string error(const std::optional<std::string>& commandId,
        const std::string& code, const std::string& message);

    // Состояние книги заявок для PRINT (раздел 3.4): buy по убыванию цены,
    // sell по возрастанию, внутри уровня — в порядке поступления. Заказы
    // берутся уже в этом порядке из OrderBook::buyOrders()/sellOrders() —
    // свой обход уровней здесь не пишется. commandId — эхо, если клиент его
    // прислал (PRINT не требует command_id, но раздел 2 контракта требует
    // вернуть его, если он всё же был).
    static std::string printBook(const OrderBook& book,
        const std::optional<std::string>& commandId = std::nullopt);
};

}
