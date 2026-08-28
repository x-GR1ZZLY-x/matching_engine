#include<gtest/gtest.h>
#include<optional>
#include<string>
#include"command_repository.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"pg_transaction.hpp"
#include"test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

// Решение "без ON CONFLICT" (задача 06, п.3): повторное сохранение команды
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
