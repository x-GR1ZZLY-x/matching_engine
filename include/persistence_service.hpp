#pragma once

#include<string>
#include"command_repository.hpp"
#include"execution_result.hpp"
#include"order_repository.hpp"
#include"pg_connection.hpp"
#include"trade_repository.hpp"

namespace matching_engine{

// Граница транзакции для одной изменяющей команды (REQ-TX-01/02): пишет
// заявки, сделки и запись об обработанной команде одной PgTransaction —
// либо всё, либо ничего. Сама не решает, что писать и как это сериализовать
// в JSON — только вызывает репозитории задачи 06 по очереди в порядке,
// заданном внешними ключами схемы (заявки прежде сделок, docs/tasks/task-07.md п.0).
class PersistenceService{
public:
    // Бросает PersistenceError, если сохранение не удалось. Транзакция к
    // этому моменту уже откатилась собственным RAII-деструктором
    // PgTransaction (ROLLBACK) — здесь только оборачивание исключения в
    // тип, который Application обязан не проглотить общим catch, а
    // пробросить дальше и завершить процесс (см. Application::processCommand).
    //
    // record собирает commandId/commandType/status/result одним аргументом
    // — четыре подряд идущих std::string оставляли риск молча переставить
    // их местами на вызове; типа ProcessedCommandRecord для этого
    // достаточно, он уже существует под нужную запись processed_commands.
    void save(PgConnection& connection, const ExecutionResult& result,
        const ProcessedCommandRecord& record);

private:
    OrderRepository orderRepo_;
    TradeRepository tradeRepo_;
    CommandRepository commandRepo_;
};

}
