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

    // Пакетная запись (задача 12, REQ-OPT-03): многострочный INSERT на весь
    // список записей пакета, за один или несколько execute() (см.
    // sql_batch_insert.hpp — список режется по числу строк, если иначе
    // запрос превысил бы протокольный предел параметров PostgreSQL), без
    // ON CONFLICT — как и у save(), повторный command_id внутри пакета не
    // ожидается (кеш идемпотентности не пускает его в буфер CommandProcessor
    // дважды) и должен упасть на первичном ключе, если всё же произошёл.
    // Пустой список — no-op.
    void saveBatch(PgConnection& connection, const std::vector<ProcessedCommandRecord>& records);

    std::optional<ProcessedCommandRecord> findById(PgConnection& connection,
        const std::string& commandId);

    // Прогрев кеша идемпотентности на старте (задача 08).
    std::vector<ProcessedCommandRecord> loadAll(PgConnection& connection);
};

}
