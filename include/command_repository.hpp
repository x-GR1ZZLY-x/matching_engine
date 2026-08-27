#pragma once

#include<optional>
#include<string>
#include<vector>
#include"pg_connection.hpp"

namespace matching_engine{

// Строка таблицы processed_commands. result — уже сериализованный JSON
// результата исполнения (сериализация — задача 07), репозиторий его не
// разбирает и не строит, только хранит и возвращает как строку. Столбец
// result — JSONB и nullable, отсюда std::optional<std::string>.
struct ProcessedCommandRecord{
    std::string commandId;
    std::string commandType;
    std::string status;
    std::optional<std::string> result;
};

// Отображает записи журнала обработанных команд. Как и остальные
// репозитории, соединение получает параметром на каждый вызов и не хранит
// его полем; транзакциями не управляет.
class CommandRepository{
public:
    // Обычная вставка, без ON CONFLICT: повторный command_id должен
    // приводить к ошибке (нарушению уникальности первичного ключа), а не
    // молча перезаписывать строку — от дублей защищает кеш выше по стеку
    // (задача 07/08), и если запрос всё же дошёл сюда с существующим
    // идентификатором, значит кеш не сработал, и это должно быть видно.
    void save(PgConnection& connection, const std::string& commandId,
        const std::string& commandType, const std::string& status,
        const std::optional<std::string>& result);

    std::optional<ProcessedCommandRecord> findById(PgConnection& connection,
        const std::string& commandId);

    // Прогрев кеша идемпотентности на старте (задача 08).
    std::vector<ProcessedCommandRecord> loadAll(PgConnection& connection);
};

}
