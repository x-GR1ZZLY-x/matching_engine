#include"command_repository.hpp"
#include"sql_batch_insert.hpp"
#include<optional>
#include<string>
#include<vector>

namespace matching_engine{

namespace{

ProcessedCommandRecord readRecord(const PgResult& result, int row){
    ProcessedCommandRecord record;
    record.commandId = result.getValue(row, 0);
    record.commandType = result.getValue(row, 1);
    record.status = result.getValue(row, 2);
    record.result = result.isNull(row, 3)
        ? std::nullopt
        : std::optional<std::string>(result.getValue(row, 3));
    return record;
}

}

void CommandRepository::save(PgConnection& connection, const std::string& commandId,
    const std::string& commandType, const std::string& status,
    const std::optional<std::string>& result){

    std::vector<std::optional<std::string>> params{commandId, commandType, status, result};

    // Выполняется на каждой изменяющей команде — подготовленный запрос
    // экономит повторный разбор и планирование этой вставки.
    connection.executePrepared(
        "command_repository_insert",
        "INSERT INTO processed_commands (command_id, command_type, status, result) "
        "VALUES ($1, $2, $3, $4)",
        params);
}

void CommandRepository::saveBatch(PgConnection& connection,
    const std::vector<ProcessedCommandRecord>& records){
    // Резка на несколько execute() при превышении предела параметров
    // протокола и построение плейсхолдеров — общие для трёх репозиториев,
    // см. sql_batch_insert.hpp; rowCount == 0 там же обрабатывается как no-op.
    executeBatchedInsert(connection, records.size(), 4,
        "INSERT INTO processed_commands (command_id, command_type, status, result) VALUES ",
        "",
        [&records](std::size_t row) -> std::vector<std::optional<std::string>>{
            const ProcessedCommandRecord& record = records[row];
            return {record.commandId, record.commandType, record.status, record.result};
        });
}

std::optional<ProcessedCommandRecord> CommandRepository::findById(PgConnection& connection,
    const std::string& commandId){

    PgResult result = connection.execute(
        "SELECT command_id, command_type, status, result FROM processed_commands "
        "WHERE command_id = $1",
        {std::optional<std::string>(commandId)});

    if(result.rowCount() == 0){
        return std::nullopt;
    }

    return readRecord(result, 0);
}

std::vector<ProcessedCommandRecord> CommandRepository::loadAll(PgConnection& connection){
    PgResult result = connection.execute(
        "SELECT command_id, command_type, status, result FROM processed_commands");

    std::vector<ProcessedCommandRecord> records;
    records.reserve(static_cast<size_t>(result.rowCount()));

    for(int row = 0; row < result.rowCount(); ++row){
        records.push_back(readRecord(result, row));
    }

    return records;
}

}
