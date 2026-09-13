#include<gtest/gtest.h>
#include<filesystem>
#include<fstream>
#include<optional>
#include<string>
#include<unistd.h>
#include"pg_connection.hpp"
#include"pg_result.hpp"
#include"schema.hpp"
#include"exceptions.hpp"
#include"test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

namespace {

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

// RAII-хелпер: временно меняет права доступа к пути и восстанавливает
// исходные при разрушении объекта, даже если тело теста упало раньше.
class ScopedPermissions {
public:
    ScopedPermissions(const std::filesystem::path& path, std::filesystem::perms newPerms)
        : path_(path) {
        std::error_code ec;
        original_ = std::filesystem::status(path_, ec).permissions();
        std::filesystem::permissions(path_, newPerms, std::filesystem::perm_options::replace, ec);
        if(ec){
            ADD_FAILURE() << "Cannot change permissions of " << path_ << ": " << ec.message();
        }
    }

    ~ScopedPermissions(){
        std::error_code ec;
        std::filesystem::permissions(path_, original_, std::filesystem::perm_options::replace, ec);
    }

    ScopedPermissions(const ScopedPermissions&) = delete;
    ScopedPermissions& operator=(const ScopedPermissions&) = delete;

private:
    std::filesystem::path path_;
    std::filesystem::perms original_;
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

// Каталог существует, но без единого *.sql файла — отдельная ветка от
// "каталога не существует" (MissingDirectoryThrowsDatabaseError): здесь
// is_directory() истинен, но список files пуст.
TEST(SchemaTest, EmptyDirectoryThrowsDatabaseError){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    ScopedSchemaDir dir("empty");
    EXPECT_THROW(applySchema(*connOpt, dir.path()), DatabaseError);
}

// Файл, состоящий только из пробельных символов, — isBlank() истинен, файл
// пропускается без выполнения (continue), а не приводит к ошибке пустого
// запроса.
TEST(SchemaTest, BlankSqlFileIsSkippedWithoutError){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    ScopedSchemaDir dir("blank_file");
    dir.writeFile("001_blank.sql", "   \n\t\n  \n");
    dir.writeFile("002_real.sql",
        "CREATE TEMPORARY TABLE schema_test_blank (id INTEGER);\n");

    EXPECT_NO_THROW(applySchema(conn, dir.path()));

    PgResult result = conn.execute("SELECT COUNT(*) FROM schema_test_blank");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "0");
}

// Каталог существует, но не является директорией (обычный файл) — тот же
// код (!isDirectory), но другой физический сценарий, чем отсутствующий путь.
TEST(SchemaTest, PathIsRegularFileThrowsDatabaseError){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    const std::filesystem::path filePath = std::filesystem::temp_directory_path() /
        "matching_engine_schema_test_not_a_directory.tmp";
    {
        std::ofstream file(filePath);
        file << "not a directory";
    }

    EXPECT_THROW(applySchema(*connOpt, filePath.string()), DatabaseError);

    std::filesystem::remove(filePath);
}

// Файл найден каталогом и отобран по расширению, но недоступен на чтение —
// отдельная ветка от "каталог не открылся": readFile() обязана дать
// DatabaseError через собственную проверку is_open(), а не свалиться в
// неопределённое поведение или проглотить ошибку. Права восстанавливаются
// в деструкторе ScopedPermissions даже при падении теста.
TEST(SchemaTest, UnreadableSqlFileThrowsDatabaseError){
    if(geteuid() == 0){
        GTEST_SKIP() << "Тест непоказателен под root: проверка прав доступа не работает";
    }
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    ScopedSchemaDir dir("unreadable_file");
    dir.writeFile("001_unreadable.sql", "SELECT 1;\n");
    const std::filesystem::path filePath =
        std::filesystem::path(dir.path()) / "001_unreadable.sql";

    ScopedPermissions noPerms(filePath, std::filesystem::perms::none);

    try{
        applySchema(*connOpt, dir.path());
        FAIL() << "applySchema must throw when a selected .sql file cannot be opened for reading";
    } catch(const DatabaseError& e){
        EXPECT_NE(std::string(e.what()).find("Cannot open schema file"), std::string::npos);
    }
}

// Каталог схемы существует, но обход к нему запрещён правами родительского
// каталога: std::filesystem::is_directory возвращает false и заполняет
// error_code (EACCES), а не бросает исключение сама — applySchema обязана
// проверить именно ec и дать DatabaseError, а не тихо продолжить с
// isDirectory == false, как в случае отсутствующего пути. До этого теста
// ветка "ec выставлен" не срабатывала ни разу.
TEST(SchemaTest, InaccessibleParentDirectoryThrowsDatabaseError){
    if(geteuid() == 0){
        GTEST_SKIP() << "Тест непоказателен под root: проверка прав доступа не работает";
    }
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    const std::filesystem::path outer = std::filesystem::temp_directory_path() /
        ("matching_engine_schema_test_inaccessible_outer_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directory(outer, ec);
    ASSERT_FALSE(ec) << "Cannot create outer directory: " << ec.message();
    const std::filesystem::path inner = outer / "schema";
    std::filesystem::create_directory(inner, ec);
    ASSERT_FALSE(ec) << "Cannot create inner directory: " << ec.message();

    {
        // Без права на выполнение (обход) внешнего каталога ядро не даёт
        // проверить, что находится внутри него.
        ScopedPermissions noTraverse(outer, std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write);

        // Сообщение обязано отличаться от ветки "каталог не найден"
        // (DatabaseError бросается и там, и здесь): без проверки текста
        // тест не отличил бы срабатывание проверки ec от ложного
        // прохождения через ветку !isDirectory с тем же типом исключения.
        try{
            applySchema(*connOpt, inner.string());
            FAIL() << "applySchema must throw when is_directory reports an error via ec";
        } catch(const DatabaseError& e){
            EXPECT_NE(std::string(e.what()).find("Cannot access schema directory"),
                std::string::npos);
        }
    }

    std::filesystem::remove_all(outer, ec);
}

// Символьная ссылка внутри каталога схемы указывает на файл за закрытым
// (без права на исполнение/обход) подкаталогом: разыменование ссылки при
// вызове directory_entry::is_regular_file() даёт EACCES — настоящую ошибку
// filesystem, а не "файл не найден" (последнюю libstdc++ не считает
// ошибкой и не бросает на ней исключение вовсе, что и обнаружилось при
// попытке смоделировать этот сценарий простой висячей ссылкой). Именно
// EACCES обязан попасть в catch(fs::filesystem_error) и превратиться в
// DatabaseError — до этого теста этот catch-блок ни разу не срабатывал.
// Рядом кладётся валидный .sql файл, чтобы отличить это от ветки
// "No .sql files found" (files.empty()), которая бросает DatabaseError с
// другим текстом.
TEST(SchemaTest, SymlinkThroughInaccessibleDirectoryThrowsDatabaseError){
    if(geteuid() == 0){
        GTEST_SKIP() << "Тест непоказателен под root: проверка прав доступа не работает";
    }
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    const std::filesystem::path outer = std::filesystem::temp_directory_path() /
        ("matching_engine_schema_test_symlink_outer_" + std::to_string(::getpid()));
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directory(outer, ec)) << ec.message();

    const std::filesystem::path hidden = outer / "hidden";
    ASSERT_TRUE(std::filesystem::create_directory(hidden, ec)) << ec.message();
    {
        std::ofstream target(hidden / "target.sql");
        target << "SELECT 1;\n";
    }

    const std::filesystem::path schemaDir = outer / "schema";
    ASSERT_TRUE(std::filesystem::create_directory(schemaDir, ec)) << ec.message();
    {
        std::ofstream validFile(schemaDir / "000_valid.sql");
        validFile << "SELECT 1;\n";
    }
    std::filesystem::create_symlink(hidden / "target.sql", schemaDir / "001_link.sql", ec);
    if(ec){
        GTEST_SKIP() << "Файловая система не поддерживает символьные ссылки: " << ec.message();
    }

    {
        // Без права на обход "hidden" разыменование ссылки на файл внутри
        // неё падает с EACCES.
        ScopedPermissions noTraverse(hidden, std::filesystem::perms::none);

        try{
            applySchema(*connOpt, schemaDir.string());
            FAIL() << "applySchema must throw when resolving a directory entry fails "
                "with a real filesystem error (EACCES)";
        } catch(const DatabaseError& e){
            EXPECT_NE(std::string(e.what()).find("Cannot read schema directory"),
                std::string::npos);
        }
    }

    std::filesystem::remove_all(outer, ec);
}
