#include<gtest/gtest.h>
#include<optional>
#include<string>
#include<type_traits>
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"pg_result.hpp"
#include"pg_transaction.hpp"
#include"test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

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

// Задача 10, находка 4: isConnected() сам по себе не был покрыт ни одним
// тестом — HealthReportsNonConnectedDatabaseWhenNoConnectionIsConfigured
// (tests/network_tests.cpp) проверяет только ветку "connection == nullptr" в
// RequestRouter, и мысленная инъекция "return true;" внутри тела
// isConnected() осталась бы незамеченной. На живом, только что открытом
// соединении isConnected() обязан вернуть true; после того как соединение
// оборвано и это обнаружено провалом следующей команды, — false.
TEST(PgWrapperIntegrationTest, IsConnectedReflectsActualConnectionState){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    EXPECT_TRUE(conn.isConnected());

    // Обрывает собственное соединение со стороны сервера. Сам этот вызов
    // может и не вернуть результат клиенту (бэкенд способен завершиться
    // раньше, чем успеет отправить ответ) — оба исхода здесь ожидаемы, тест
    // проверяет не его, а команду ниже.
    try{
        conn.execute("SELECT pg_terminate_backend(pg_backend_pid())");
    }catch(const DatabaseError&){
    }

    // PQstatus не опрашивает сеть (см. докстроку isConnected() в
    // pg_connection.hpp) — он переходит в CONNECTION_BAD только по итогам
    // неудачной команды, поэтому оборванность соединения обнаруживается
    // именно здесь, а не в момент самого обрыва.
    EXPECT_THROW(conn.execute("SELECT 1"), DatabaseError);
    EXPECT_FALSE(conn.isConnected());
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
