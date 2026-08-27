#include"persistence_service.hpp"
#include<optional>
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

}
