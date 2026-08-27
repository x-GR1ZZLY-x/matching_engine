#include<gtest/gtest.h>
#include<cstdlib>
#include<optional>
#include<string>
#include"command_repository.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"pg_transaction.hpp"

using namespace matching_engine;

namespace {

std::string envOrDefault(const char* name, const std::string& fallback){
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

std::string g_lastConnectFailure = "переменная MATCHING_ENGINE_DB_PASSWORD не задана";

std::optional<PgConnection> tryConnect(){
    const char* password = std::getenv("MATCHING_ENGINE_DB_PASSWORD");
    if(!password || password[0] == '\0'){
        g_lastConnectFailure = "переменная MATCHING_ENGINE_DB_PASSWORD не задана";
        return std::nullopt;
    }

    std::string host = envOrDefault("MATCHING_ENGINE_DB_HOST", "127.0.0.1");
    std::string port = envOrDefault("MATCHING_ENGINE_DB_PORT", "5432");
    std::string dbname = envOrDefault("MATCHING_ENGINE_DB_NAME", "matching_engine");
    std::string user = envOrDefault("MATCHING_ENGINE_DB_USER", "engine");

    try{
        return PgConnection(host, port, dbname, user, password);
    } catch(const std::exception& ex){
        g_lastConnectFailure = ex.what();
        return std::nullopt;
    }
}

}

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
