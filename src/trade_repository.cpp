#include"trade_repository.hpp"
#include"exceptions.hpp"
#include"sql_batch_insert.hpp"
#include<optional>
#include<string>
#include<vector>

namespace matching_engine{

namespace{

// См. пояснение у одноимённого помощника в order_repository.cpp: std::stoi
// бросает std::invalid_argument/std::out_of_range, не входящие в иерархию
// MatchingEngineError, и не сообщает, что строка пришла из БД.
int parseInt(const std::string& text, const std::string& context){
    try{
        return std::stoi(text);
    } catch(const std::exception&){
        throw DatabaseError("Failed to parse integer from database (" + context +
            "): '" + text + "'");
    }
}

}

void TradeRepository::insert(PgConnection& connection, const Trade& trade){
    std::vector<std::optional<std::string>> params{
        std::to_string(trade.getBuyOrderId()),
        std::to_string(trade.getSellOrderId()),
        std::to_string(trade.getPrice()),
        std::to_string(trade.getQuantity())
    };

    // Выполняется на каждой сделке — подготовленный запрос
    // экономит повторный разбор и планирование этой вставки.
    connection.executePrepared(
        "trade_repository_insert",
        "INSERT INTO trades (buy_order_id, sell_order_id, price, quantity) "
        "VALUES ($1, $2, $3, $4)",
        params);
}

void TradeRepository::insertBatch(PgConnection& connection, const std::vector<Trade>& trades){
    // Резка на несколько execute() при превышении предела параметров
    // протокола и построение плейсхолдеров — общие для трёх репозиториев,
    // см. sql_batch_insert.hpp; rowCount == 0 там же обрабатывается как no-op.
    executeBatchedInsert(connection, trades.size(), 4,
        "INSERT INTO trades (buy_order_id, sell_order_id, price, quantity) VALUES ",
        "",
        [&trades](std::size_t row) -> std::vector<std::optional<std::string>>{
            const Trade& trade = trades[row];
            return {
                std::to_string(trade.getBuyOrderId()),
                std::to_string(trade.getSellOrderId()),
                std::to_string(trade.getPrice()),
                std::to_string(trade.getQuantity())
            };
        });
}

std::vector<Trade> TradeRepository::loadAll(PgConnection& connection){
    PgResult result = connection.execute(
        "SELECT buy_order_id, sell_order_id, price, quantity FROM trades "
        "ORDER BY trade_id ASC");

    std::vector<Trade> trades;
    trades.reserve(static_cast<size_t>(result.rowCount()));

    for(int row = 0; row < result.rowCount(); ++row){
        std::string rowContext = "trades, row " + std::to_string(row);
        int buyOrderId = parseInt(result.getValue(row, 0), "trades.buy_order_id, " + rowContext);
        int sellOrderId = parseInt(result.getValue(row, 1), "trades.sell_order_id, " + rowContext);
        int price = parseInt(result.getValue(row, 2), "trades.price, " + rowContext);
        int quantity = parseInt(result.getValue(row, 3), "trades.quantity, " + rowContext);
        trades.emplace_back(buyOrderId, sellOrderId, price, quantity);
    }

    return trades;
}

}
