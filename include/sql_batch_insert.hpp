#pragma once

#include<algorithm>
#include<cstddef>
#include<functional>
#include<optional>
#include<string>
#include<vector>
#include"pg_connection.hpp"

namespace matching_engine{

// Протокольный предел PostgreSQL: расширенный протокол несёт число
// параметров запроса в 16-битном поле, поэтому один PQexecParams/execute()
// не может принять больше 65535 значений (задача 12, правка 1).
constexpr std::size_t kMaxSqlParameters = 65535;

// Общий для OrderRepository::saveBatch, TradeRepository::insertBatch и
// CommandRepository::saveBatch помощник: строит "($1, $2, ...), (...), ..."
// для rowCount строк по columnCount значений в каждой и выполняет
// queryPrefix + valuesClause + querySuffix через connection.execute().
//
// Если rowCount * columnCount превысил бы kMaxSqlParameters, список режется
// на куски по (kMaxSqlParameters / columnCount) строк, и каждый кусок
// отправляется отдельным execute() — но все они выполняются на одном и том
// же connection, поэтому остаются внутри транзакции, которую держит
// вызывающий: режется сам SQL-запрос, а не граница транзакции.
//
// rowParams(row) обязан вернуть ровно columnCount значений для строки row
// (0-based индекс по всему rowCount, а не по текущему куску), в порядке
// колонок VALUES. rowCount == 0 — no-op, ни один execute() не выполняется.
inline void executeBatchedInsert(PgConnection& connection, std::size_t rowCount,
    std::size_t columnCount, const std::string& queryPrefix, const std::string& querySuffix,
    const std::function<std::vector<std::optional<std::string>>(std::size_t)>& rowParams){

    if(rowCount == 0){
        return;
    }

    const std::size_t maxRowsPerQuery = kMaxSqlParameters / columnCount;

    for(std::size_t chunkStart = 0; chunkStart < rowCount; chunkStart += maxRowsPerQuery){
        const std::size_t chunkEnd = std::min(rowCount, chunkStart + maxRowsPerQuery);

        std::string valuesClause;
        std::vector<std::optional<std::string>> params;
        params.reserve((chunkEnd - chunkStart) * columnCount);

        for(std::size_t row = chunkStart; row < chunkEnd; ++row){
            if(row > chunkStart){
                valuesClause += ", ";
            }
            const std::size_t base = (row - chunkStart) * columnCount;
            valuesClause += "(";
            for(std::size_t col = 0; col < columnCount; ++col){
                if(col > 0){
                    valuesClause += ", ";
                }
                valuesClause += "$" + std::to_string(base + col + 1);
            }
            valuesClause += ")";

            for(auto& value : rowParams(row)){
                params.push_back(std::move(value));
            }
        }

        connection.execute(queryPrefix + valuesClause + querySuffix, params);
    }
}

}
