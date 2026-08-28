#include"recovery_service.hpp"
#include<memory>
#include<string>
#include<vector>
#include"command_repository.hpp"
#include"logger.hpp"
#include"order_repository.hpp"
#include"sequence_generator.hpp"

namespace matching_engine{

void recoverState(PgConnection& connection, CommandProcessor& processor){
    Logger::instance().info("Recovering order book from database");

    OrderRepository orderRepo;
    std::vector<std::shared_ptr<Order>> activeOrders = orderRepo.loadActive(connection);
    for(const auto& order : activeOrders){
        processor.restoreOrder(order);
    }
    Logger::instance().info("Restored " + std::to_string(activeOrders.size()) +
        " active order(s) into the book");

    // Максимум по всей таблице, а не по активным заявкам, которые вернул
    // loadActive() выше — иначе исполненная или отменённая заявка с бо́льшим
    // номером осталась бы невидимой, и первая же вставка после старта
    // столкнулась бы с UNIQUE на orders.sequence_number.
    SequenceGenerator::instance().reset(orderRepo.maxSequenceNumber(connection) + 1);

    CommandRepository commandRepo;
    std::vector<ProcessedCommandRecord> records = commandRepo.loadAll(connection);
    for(const auto& record : records){
        processor.warmCache(record.commandId, record.result);
    }
    Logger::instance().info("Warmed command cache with " + std::to_string(records.size()) +
        " record(s)");

    Logger::instance().info("Matching engine ready to accept commands");
}

}
