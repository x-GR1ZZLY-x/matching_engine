#pragma once

#include<string>
#include<vector>
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

    // Пакетная запись при --replay --batch (задача 12, REQ-OPT-03): весь
    // накопленный пакет command'ов — одна PgTransaction, а не одна на
    // команду (это отдельный от save() путь, штатный save() не меняется —
    // критерий 3 задачи 12). results и records — параллельные векторы (как
    // pendingResults_/pendingRecords_ у CommandProcessor), по одному
    // ExecutionResult и одной ProcessedCommandRecord на команду пакета, в
    // порядке обработки.
    //
    // Заявки всех команд пакета пишутся раньше сделок всех команд пакета
    // (trades.buy_order_id/sell_order_id -> orders(order_id) внешним
    // ключом, а сделка из середины пакета может ссылаться на заявку,
    // добавленную более ранней командой того же пакета) — в отличие от
    // save(), где заявки и сделки чередуются по одной команде за раз, здесь
    // сначала собираются и пишутся ВСЕ заявки пакета, затем ВСЕ сделки.
    // Свёртку повторных изменений одного order_id (обязательна: PostgreSQL
    // не может обновить одну строку дважды за один ON CONFLICT DO UPDATE,
    // ошибка 21000) делает OrderRepository::saveBatch.
    //
    // Бросает PersistenceError, как и save() — тот же фатальный класс ошибок.
    void saveBatch(PgConnection& connection, const std::vector<ExecutionResult>& results,
        const std::vector<ProcessedCommandRecord>& records);

private:
    OrderRepository orderRepo_;
    TradeRepository tradeRepo_;
    CommandRepository commandRepo_;
};

}
