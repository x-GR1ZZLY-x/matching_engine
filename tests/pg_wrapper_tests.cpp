#include<gtest/gtest.h>
#include<cstdlib>
#include<optional>
#include<string>
#include<type_traits>
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"pg_result.hpp"
#include"pg_transaction.hpp"

using namespace matching_engine;

namespace {

std::string envOrDefault(const char* name, const std::string& fallback){
    const char* value = std::getenv(name);
    return value ? std::string(value) : fallback;
}

// Причина последнего неудачного tryConnect() — выводится через GTEST_SKIP(),
// чтобы отличить "сервер не запущен" от опечатки в пароле или имени БД.
std::string g_lastConnectFailure = "переменная MATCHING_ENGINE_DB_PASSWORD не задана";

// Пытается подключиться к тестовой базе. Возвращает nullopt, если БД
// недоступна или пароль не задан — тогда интеграционные тесты пропускаются.
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

// ─── Проверки на уровне типов (не требуют БД) ─────────────────────────────

TEST(PgConnectionTest, NotCopyable){
    static_assert(!std::is_copy_constructible_v<PgConnection>);
    static_assert(!std::is_copy_assignable_v<PgConnection>);
}

TEST(PgConnectionTest, Movable){
    static_assert(std::is_move_constructible_v<PgConnection>);
    static_assert(std::is_move_assignable_v<PgConnection>);
}

TEST(PgResultTest, NotCopyable){
    static_assert(!std::is_copy_constructible_v<PgResult>);
    static_assert(!std::is_copy_assignable_v<PgResult>);
}

TEST(PgResultTest, Movable){
    static_assert(std::is_move_constructible_v<PgResult>);
    static_assert(std::is_move_assignable_v<PgResult>);
}

TEST(PgTransactionTest, NotCopyableNotMovable){
    static_assert(!std::is_copy_constructible_v<PgTransaction>);
    static_assert(!std::is_copy_assignable_v<PgTransaction>);
    static_assert(!std::is_move_constructible_v<PgTransaction>);
    static_assert(!std::is_move_assignable_v<PgTransaction>);
}

// ─── Интеграционные тесты с реальной БД ───────────────────────────────────

TEST(PgWrapperIntegrationTest, MoveConstructorTransfersOwnershipToDestination){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    PgConnection moved(std::move(*connOpt));
    PgResult result = moved.execute("SELECT 1");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "1");
}

// Проверяет пропущенную ранее часть контракта перемещения: объект,
// из которого переместили (source ниже), не наследует "сломанное" состояние —
// его можно безопасно уничтожить и, что важнее, безопасно переприсвоить через
// перемещающее присваивание, которое до этого теста не было покрыто вовсе.
TEST(PgWrapperIntegrationTest, MoveAssignmentWorksAndSourceStaysReassignableAfterMove){
    auto firstOpt = tryConnect();
    auto secondOpt = tryConnect();
    if(!firstOpt || !secondOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    // Перемещающее присваивание: firstOpt должен освободить свой прежний
    // ресурс и принять соединение из secondOpt.
    *firstOpt = std::move(*secondOpt);
    PgResult result = firstOpt->execute("SELECT 1");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "1");

    // secondOpt теперь перемещён-из. Его можно безопасно переприсвоить...
    auto thirdOpt = tryConnect();
    if(!thirdOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    *secondOpt = std::move(*thirdOpt);
    PgResult reassigned = secondOpt->execute("SELECT 1");
    ASSERT_EQ(reassigned.rowCount(), 1);
    EXPECT_EQ(reassigned.getValue(0, 0), "1");

    // ...и корректно разрушить по выходу из области видимости (проверяется
    // неявно деструкторами firstOpt/secondOpt/thirdOpt в конце теста).
}

TEST(PgWrapperIntegrationTest, RollbackWithoutCommit){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    conn.execute("CREATE TEMPORARY TABLE pg_wrapper_rollback_test (id INTEGER)");

    {
        PgTransaction tx(conn);
        conn.execute("INSERT INTO pg_wrapper_rollback_test (id) VALUES ($1)",
            {std::optional<std::string>("1")});
        // commit() не вызывается — деструктор должен откатить вставку.
    }

    PgResult result = conn.execute("SELECT COUNT(*) FROM pg_wrapper_rollback_test");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "0");
}

TEST(PgWrapperIntegrationTest, CommitPersists){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    conn.execute("CREATE TEMPORARY TABLE pg_wrapper_commit_test (id INTEGER)");

    {
        PgTransaction tx(conn);
        conn.execute("INSERT INTO pg_wrapper_commit_test (id) VALUES ($1)",
            {std::optional<std::string>("42")});
        tx.commit();
    }

    PgResult result = conn.execute("SELECT id FROM pg_wrapper_commit_test");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "42");
}

// Критерий 9 задачи 06: перемещение соединения с живой (не разрушенной)
// PgTransaction должно быть замечено громко, а не оставлять транзакцию
// висящей на сервере молча. Сценарий воспроизводит ревью дословно.
TEST(PgWrapperIntegrationTest, MoveConstructorWithActiveTransactionThrows){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    PgTransaction tx(conn);
    EXPECT_THROW(PgConnection moved(std::move(conn)), DatabaseError);

    // Бросок должен произойти до фактического перемещения: conn остаётся
    // рабочим соединением, и ROLLBACK в деструкторе tx отработает как обычно.
    PgResult result = conn.execute("SELECT 1");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "1");
}

TEST(PgWrapperIntegrationTest, MoveAssignmentWithActiveTransactionThrows){
    auto sourceOpt = tryConnect();
    auto destOpt = tryConnect();
    if(!sourceOpt || !destOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    PgTransaction tx(*sourceOpt);
    EXPECT_THROW(*destOpt = std::move(*sourceOpt), DatabaseError);

    // Оба соединения остались рабочими — перемещения не произошло.
    PgResult result = sourceOpt->execute("SELECT 1");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "1");
}

TEST(PgWrapperIntegrationTest, NullParameterReachesServerAsNull){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    conn.execute("CREATE TEMPORARY TABLE pg_wrapper_null_test (price INTEGER)");
    conn.execute("INSERT INTO pg_wrapper_null_test (price) VALUES ($1)",
        {std::optional<std::string>(std::nullopt)});

    PgResult result = conn.execute("SELECT price FROM pg_wrapper_null_test");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_TRUE(result.isNull(0, 0));
}
