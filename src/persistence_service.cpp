#include"persistence_service.hpp"
#include<optional>
#include<string>
#include"exceptions.hpp"
#include"pg_transaction.hpp"

namespace matching_engine{

void PersistenceService::save(PgConnection& connection, const ExecutionResult& result,
    const ProcessedCommandRecord& record){

    try{
        PgTransaction tx(connection);

        // Заявки — строго до сделок: trades.buy_order_id/sell_order_id
        // ссылаются на orders(order_id) внешним ключом. orderChanges
        // применяется по одной записи в порядке вектора, без дедупликации:
        // для одного id может быть несколько снимков (например, старый и
        // новый при MODIFY), и должна победить последняя запись — этого
        // добивается именно последовательный вызов save() по порядку,
        // а не, скажем, сбор в map по id.
        for(const auto& change : result.orderChanges){
            orderRepo_.save(connection, change);
        }
        for(const auto& trade : result.trades){
            tradeRepo_.insert(connection, trade);
        }
        commandRepo_.save(connection, record.commandId, record.commandType, record.status,
            record.result);

        tx.commit();
    } catch(const MatchingEngineError& e){
        throw PersistenceError("Failed to persist command '" + record.commandId + "': " + e.what());
    }
}

void PersistenceService::saveBatch(PgConnection& connection,
    const std::vector<ExecutionResult>& results, const std::vector<ProcessedCommandRecord>& records){

    try{
        PgTransaction tx(connection);

        // Заявки всех команд пакета собираются в один список и пишутся
        // одним многострочным запросом раньше сделок (см. докстроку
        // saveBatch в заголовке) — OrderRepository::saveBatch сам сворачивает
        // повторные изменения одного order_id, оставляя последнее.
        std::vector<OrderChange> allChanges;
        std::vector<Trade> allTrades;
        for(const auto& result : results){
            allChanges.insert(allChanges.end(), result.orderChanges.begin(),
                result.orderChanges.end());
            allTrades.insert(allTrades.end(), result.trades.begin(), result.trades.end());
        }

        orderRepo_.saveBatch(connection, allChanges);
        tradeRepo_.insertBatch(connection, allTrades);
        commandRepo_.saveBatch(connection, records);

        tx.commit();
    } catch(const MatchingEngineError& e){
        throw PersistenceError("Failed to persist a batch of " +
            std::to_string(records.size()) + " command(s): " + e.what());
    }
}

}
