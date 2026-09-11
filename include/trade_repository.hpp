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

    // Пакетная запись (REQ-OPT-03): многострочный INSERT на весь
    // список сделок пакета, за один или несколько execute() (см.
    // sql_batch_insert.hpp — список режется по числу строк, если иначе
    // запрос превысил бы протокольный предел параметров PostgreSQL). В
    // отличие от orders, у trades нет ON CONFLICT — обычная вставка, поэтому
    // свёртка по ключу не нужна, строки независимы. Пустой список — no-op.
    // Обычный execute(), не executePrepared() — форма запроса зависит от
    // числа строк.
    void insertBatch(PgConnection& connection, const std::vector<Trade>& trades);

    std::vector<Trade> loadAll(PgConnection& connection);
};

}
