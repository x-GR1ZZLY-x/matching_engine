#include<gtest/gtest.h>
#include<cstdlib>
#include<filesystem>
#include<fstream>
#include<optional>
#include<string>
#include"pg_connection.hpp"
#include"pg_result.hpp"
#include"schema.hpp"
#include"exceptions.hpp"

using namespace matching_engine;

namespace {

std::string envOrDefault(const char* name, const std::string& fallback){
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

std::string g_lastConnectFailure = "переменная MATCHING_ENGINE_DB_PASSWORD не задана";

// Пытается подключиться к тестовой базе. Возвращает nullopt, если БД
// недоступна или пароль не задан — тогда тест пропускается через GTEST_SKIP.
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

// RAII-каталог со схемой: создаёт временную директорию с *.sql файлами и
// удаляет её при разрушении.
class ScopedSchemaDir {
public:
    explicit ScopedSchemaDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() /
              ("matching_engine_schema_test_" + name + "_" + std::to_string(nextId()))){
        std::error_code ec;
        const bool created = std::filesystem::create_directory(path_, ec);
        if(ec || !created){
            ADD_FAILURE() << "Cannot create scoped schema directory " << path_
                << (ec ? ": " + ec.message() : "");
        }
    }

    ~ScopedSchemaDir(){
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedSchemaDir(const ScopedSchemaDir&) = delete;
    ScopedSchemaDir& operator=(const ScopedSchemaDir&) = delete;

    void writeFile(const std::string& fileName, const std::string& content) const{
        std::ofstream file(path_ / fileName);
        if(!file.is_open()){
            ADD_FAILURE() << "Cannot open schema test file for writing: " << (path_ / fileName);
            return;
        }
        file << content;
    }

    std::string path() const { return path_.string(); }

private:
    static int nextId(){
        static int counter = 0;
        return counter++;
    }

    std::filesystem::path path_;
};

}

TEST(SchemaTest, MissingDirectoryThrowsDatabaseError){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    EXPECT_THROW(applySchema(*connOpt, "/nonexistent/matching_engine_schema_dir"),
        DatabaseError);
}

TEST(SchemaTest, AppliesMultiStatementFileInLexicalOrder){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    ScopedSchemaDir dir("multi_statement");
    // Второй файл ссылается на таблицу из первого — порядок выполнения
    // (001 до 002) обязан быть лексическим.
    dir.writeFile("001_create.sql",
        "CREATE TEMPORARY TABLE schema_test_multi (id INTEGER, note TEXT);\n"
        "INSERT INTO schema_test_multi (id, note) VALUES (1, 'first');\n");
    dir.writeFile("002_insert.sql",
        "INSERT INTO schema_test_multi (id, note) VALUES (2, 'second');\n");

    applySchema(conn, dir.path());

    PgResult result = conn.execute("SELECT COUNT(*) FROM schema_test_multi");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "2");
}

TEST(SchemaTest, RepeatedApplicationOnIdempotentSchemaSucceeds){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    ScopedSchemaDir dir("idempotent");
    dir.writeFile("001_init.sql",
        "CREATE TEMPORARY TABLE IF NOT EXISTS schema_test_idempotent (id INTEGER PRIMARY KEY);\n");

    ASSERT_NO_THROW(applySchema(conn, dir.path()));
    EXPECT_NO_THROW(applySchema(conn, dir.path()));
}

TEST(SchemaTest, RealSchemaCreatesThreeTablesAndPartialIndex){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    // Прогоняет фактическую схему проекта (database/). MATCHING_ENGINE_SOURCE_DIR
    // задаётся в CMakeLists.txt как абсолютный путь до корня репозитория —
    // ctest запускает тесты с рабочим каталогом build/, а не корнем репозитория.
    const std::string schemaDir = std::string(MATCHING_ENGINE_SOURCE_DIR) + "/database";
    ASSERT_NO_THROW(applySchema(conn, schemaDir));

    PgResult tables = conn.execute(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema = 'public' "
        "AND table_name IN ('orders', 'trades', 'processed_commands')");
    EXPECT_EQ(tables.rowCount(), 3);

    PgResult index = conn.execute(
        "SELECT indexname FROM pg_indexes "
        "WHERE tablename = 'orders' AND indexname = 'idx_orders_active_sequence'");
    EXPECT_EQ(index.rowCount(), 1);

    // Повторный прогон на уже применённой реальной схеме не должен падать.
    EXPECT_NO_THROW(applySchema(conn, schemaDir));
}
