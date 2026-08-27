#pragma once

#include"command.hpp"
#include"command_cache.hpp"
#include"execution_result.hpp"
#include"matching_engine.hpp"
#include"order_book.hpp"
#include"persistence_service.hpp"
#include"pg_connection.hpp"

namespace matching_engine{

// Ядро ДЗ-3: связывает сопоставление (MatchingEngine), кеш идемпотентности
// (CommandCache) и сохранение в БД (PersistenceService) в единый поток
// обработки одной изменяющей команды (docs/plan.md, "Идемпотентность и
// транзакция"). Об argv и std::cout не знает — это остаётся обязанностью
// Application и ReportPrinter; JSON здесь всё же используется — для
// сериализации ExecutionResult в processed_commands.result.
class CommandProcessor{
public:
    // command — изменяющая состояние команда (ADD/CANCEL/MODIFY). PRINT
    // сюда не передаётся: он не требует ни command_id, ни кеша, ни
    // транзакции, а нуждается лишь в текущем состоянии книги — Application
    // читает его напрямую через orderBook() и печатает сама.
    //
    // connection используется только на время этого вызова и не хранится
    // полем — как и у репозиториев (см. include/order_repository.hpp),
    // это исключает сценарий обращения к соединению после его перемещения.
    //
    // Порядок: command_id отсутствует -> ParseError; command_id уже в
    // кеше -> вернуть прошлый результат, ничего не выполняя и не читая БД;
    // иначе -> сопоставление в памяти, затем одна транзакция БД, затем
    // запись результата в кеш (REQ-IDEM-01..06, REQ-TX-01/02).
    ExecutionResult process(const Command& command, PgConnection& connection);

    const OrderBook& orderBook() const noexcept;

private:
    CommandCache cache_;
    MatchingEngine engine_;
    PersistenceService persistence_;
};

}
