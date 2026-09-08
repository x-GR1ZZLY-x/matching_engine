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
    // Предел длины message в ответе об ошибке (раздел 3.5): пользовательские
    // фрагменты (например, текст InvalidOrderValueError) не ограничены сами
    // по себе, и без верхней границы здесь длинное значение раздуло бы ответ
    // так же, как неограниченный эхо command_id. Публичный: на этой
    // константе строится нижняя граница server.max_message_size (раздел 4.1,
    // config.cpp) — сервер обязан быть в состоянии сообщить об ошибке даже
    // тогда, когда полноценный ответ не помещается в лимит.
    static constexpr std::size_t kMaxMessageLength = 512;

    // Предел длины command_id (раздел 3.1): длиннее — запрос отвергается как
    // INVALID_REQUEST ещё до разбора команды, сам command_id не эхируется.
    // Лежит здесь, а не в RequestRouter: ограничивает форму ответа (эхо
    // command_id не может раздуть ответ без предела), а не путь
    // маршрутизации; RequestRouter лишь читает эту константу при проверке
    // длины входящего command_id. Публичный по той же причине, что и
    // kMaxMessageLength: на нём строится нижняя граница server.max_message_size
    // (раздел 4.1, config.cpp — через maxErrorResponseSize() ниже).
    static constexpr std::size_t kMaxCommandIdLength = 128;

    // Обрезает value до не более maxLength байт и добавляет "...", если
    // обрезка произошла. Если байт maxLength (первый исключаемый байт)
    // оказывается продолжением многобайтовой UTF-8 последовательности
    // (старшие биты 10xxxxxx), граница отступает назад до начала этой
    // последовательности и отбрасывает её целиком — иначе substr() по числу
    // байт мог бы оставить в строке недопустимый хвост, на котором
    // nlohmann::json::dump() бросает json::type_error (общая причина
    // усечения и в error(), и в RequestRouter для текста "type" в
    // сообщении об ошибке — раньше это была одна и та же логика,
    // продублированная в двух файлах).
    static std::string truncateUtf8(const std::string& value, std::size_t maxLength);

    // Максимальный размер ответа error() среди всех допустимых входов
    // (docs/task4/02-network-protocol.md, раздел 4.1): вызывает саму
    // error() на входе, который экранируется nlohmann::json::dump()
    // сильнее всего (управляющие байты дают шесть символов вместо одного),
    // а не воспроизводит формат ответа вручную — иначе граница могла бы
    // разойтись с тем, что реально шлёт error(). Используется config.cpp
    // как нижняя граница server.max_message_size.
    static std::size_t maxErrorResponseSize();

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
