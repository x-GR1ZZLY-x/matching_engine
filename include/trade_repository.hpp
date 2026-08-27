#pragma once

#include<vector>
#include"pg_connection.hpp"
#include"trade.hpp"

namespace matching_engine{

// Отображает Trade на таблицу trades. Как и OrderRepository, соединение
// получает параметром на каждый вызов и не хранит его полем; транзакциями
// не управляет.
class TradeRepository{
public:
    // trade_id выдаёт база (BIGSERIAL, см. database/001_init.sql) —
    // приложение его не придумывает и при вставке не передаёт. Trade
    // (include/trade.hpp) поля с идентификатором вообще не имеет.
    void insert(PgConnection& connection, const Trade& trade);

    std::vector<Trade> loadAll(PgConnection& connection);
};

}
