#include<gtest/gtest.h>
#include<nlohmann/json.hpp>
#include<optional>
#include<string>
#include<vector>
#include"command_repository.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"pg_result.hpp"
#include"pg_transaction.hpp"
#include"test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

// Решение "без ON CONFLICT": повторное сохранение команды
// с тем же идентификатором должно приводить к ошибке нарушения уникальности,
// а не молча перезаписывать или игнорировать строку.
TEST(CommandRepositoryTest, SaveWithDuplicateIdThrowsInsteadOfIgnoring){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    CommandRepository repo;
    const std::string commandId = "task06-test-duplicate-900000601";
    repo.save(conn, commandId, "ADD", "OK", std::nullopt);

    EXPECT_THROW(repo.save(conn, commandId, "ADD", "OK", std::nullopt), DatabaseError);
}

// saveBatch() (REQ-OPT-03) не был покрыт ни одним тестом — до этой правки
// executeBatchedInsert ни разу не выполнялся с 4-колоночной формой запроса
// CommandRepository. Один из результатов — с NULL result, второй — с
// заполненным, чтобы задеть обе ветки readRecord()/isNull() ниже через
// findById.
TEST(CommandRepositoryTest, SaveBatchPersistsAllRecords){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    CommandRepository repo;
    std::vector<ProcessedCommandRecord> records{
        ProcessedCommandRecord{"task-batch-1", "ADD", "APPLIED",
            std::optional<std::string>("{\"trades\":[]}")},
        ProcessedCommandRecord{"task-batch-2", "CANCEL", "APPLIED", std::nullopt},
    };

    repo.saveBatch(conn, records);

    auto found1 = repo.findById(conn, "task-batch-1");
    ASSERT_TRUE(found1.has_value());
    EXPECT_EQ(found1->commandType, "ADD");
    // JSONB нормализует представление при хранении (например, добавляет
    // пробел после ":") — сравниваем через парсинг, а не побайтово.
    ASSERT_TRUE(found1->result.has_value());
    EXPECT_EQ(nlohmann::json::parse(*found1->result), nlohmann::json::parse("{\"trades\":[]}"));

    auto found2 = repo.findById(conn, "task-batch-2");
    ASSERT_TRUE(found2.has_value());
    EXPECT_EQ(found2->commandType, "CANCEL");
    EXPECT_FALSE(found2->result.has_value());
}

// Пустой список — no-op: вызов не должен бросать и не должен выполнять ни
// одного execute().
TEST(CommandRepositoryTest, SaveBatchWithEmptyListIsNoOp){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    CommandRepository repo;
    EXPECT_NO_THROW(repo.saveBatch(conn, {}));
}

// findById() ни разу не вызывался напрямую ни одним тестом — только через
// восстановление кеша при старте приложения. Оба исхода: команда есть,
// команды нет.
TEST(CommandRepositoryTest, FindByIdReturnsNulloptForUnknownId){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    CommandRepository repo;
    EXPECT_FALSE(repo.findById(conn, "no-such-command-id-ever").has_value());
}

// loadAll() (прогрев кеша идемпотентности на старте) тоже не был покрыт ни
// одним тестом напрямую.
TEST(CommandRepositoryTest, LoadAllIncludesSavedRecords){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    CommandRepository repo;
    repo.save(conn, "task-loadall-1", "ADD", "APPLIED", std::nullopt);

    auto all = repo.loadAll(conn);
    bool found = false;
    for(const auto& record : all){
        if(record.commandId == "task-loadall-1"){
            found = true;
            EXPECT_EQ(record.commandType, "ADD");
        }
    }
    EXPECT_TRUE(found);
}

// executeBatchedInsert (sql_batch_insert.hpp) режет один вызов saveBatch()
// на несколько execute(), если rowCount * columnCount превысил бы
// протокольный предел в 65535 параметров. Для CommandRepository (columnCount
// == 4) порог — 65535 / 4 == 16383 строк за один execute(); ни один
// существующий тест (во всех трёх репозиториях, использующих этот общий
// помощник) до сих пор не строил батч больше порога, поэтому цикл резки
//("for(chunkStart...)" в sql_batch_insert.hpp) выполнялся не более одного
// раза за весь прогон тестов. Здесь — 16400 записей, вынуждающих ровно два
// execute(): 16383 строки в первом и 17 в оставшемся куске.
TEST(CommandRepositoryTest, SaveBatchSplitsIntoMultipleChunksWhenExceedingParameterLimit){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    constexpr int kRecordCount = 16400;
    constexpr const char* kPrefix = "task-chunk-";

    std::vector<ProcessedCommandRecord> records;
    records.reserve(kRecordCount);
    for(int i = 0; i < kRecordCount; ++i){
        records.push_back(ProcessedCommandRecord{
            kPrefix + std::to_string(i), "ADD", "APPLIED", std::nullopt});
    }

    CommandRepository repo;
    EXPECT_NO_THROW(repo.saveBatch(conn, records));

    PgResult count = conn.execute(
        "SELECT COUNT(*) FROM processed_commands WHERE command_id LIKE $1",
        {std::optional<std::string>(std::string(kPrefix) + "%")});
    ASSERT_EQ(count.rowCount(), 1);
    EXPECT_EQ(count.getValue(0, 0), std::to_string(kRecordCount));
}
