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

// Перемещение соединения с живой (не разрушенной)
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

// isConnected() сам по себе не был покрыт ни одним
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

// Синтаксически неверный SQL обязан приводить к DatabaseError через ветку
// "status != PGRES_TUPLES_OK/PGRES_COMMAND_OK" в конструкторе PgResult, а не
// к неопределённому поведению или тихому пустому результату.
TEST(PgWrapperIntegrationTest, ExecuteWithInvalidSqlThrows){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    EXPECT_THROW(conn.execute("SELECT this is not valid sql"), DatabaseError);

    // Соединение остаётся рабочим после отклонённого сервером запроса —
    // следующая обычная команда должна пройти как ни в чём не бывало.
    PgResult result = conn.execute("SELECT 1");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "1");
}

// Первый вызов с новым именем готовит запрос (PQprepare + вставка в
// preparedStatements_), второй вызов с тем же именем обязан пропустить
// подготовку и сразу выполнить PQexecPrepared. Без этого теста ветка
// "имя уже подготовлено" не проходится ни разу.
TEST(PgWrapperIntegrationTest, ExecutePreparedReusesStatementOnSecondCall){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    const std::string sql = "SELECT $1::INTEGER + 1";

    PgResult first = conn.executePrepared("pg_wrapper_test_increment", sql,
        {std::optional<std::string>("41")});
    ASSERT_EQ(first.rowCount(), 1);
    EXPECT_EQ(first.getValue(0, 0), "42");

    // Тот же name, sql передаётся снова, но подготовка повторно не
    // выполняется — если бы имя не запоминалось, PQprepare с тем же именем
    // на этой сессии сервер бы отверг.
    PgResult second = conn.executePrepared("pg_wrapper_test_increment", sql,
        {std::optional<std::string>("99")});
    ASSERT_EQ(second.rowCount(), 1);
    EXPECT_EQ(second.getValue(0, 0), "100");
}

// Если PQprepare проваливается (ошибка в sql), имя не должно попасть в
// preparedStatements_ — иначе следующая попытка подготовить то же имя
// корректным sql считалась бы "уже подготовленной" и обратилась бы прямо к
// PQexecPrepared с несуществующим на сервере запросом.
TEST(PgWrapperIntegrationTest, ExecutePreparedDoesNotRememberNameAfterPrepareFailure){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    EXPECT_THROW(conn.executePrepared("pg_wrapper_test_retry", "SELECT this is not valid sql"),
        DatabaseError);

    // Тем же именем, но теперь валидным sql — обязано подготовиться и
    // выполниться заново, а не упасть на "запрос не найден".
    PgResult result = conn.executePrepared("pg_wrapper_test_retry", "SELECT 7");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "7");
}

// executeScript — единственный метод, принимающий несколько операторов через
// ';' в одной строке; execute()/executePrepared() такого не умеют
// (PQexecParams ожидает ровно один оператор).
TEST(PgWrapperIntegrationTest, ExecuteScriptRunsMultipleStatements){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    conn.executeScript(
        "CREATE TEMPORARY TABLE pg_wrapper_script_test (id INTEGER);"
        "INSERT INTO pg_wrapper_script_test (id) VALUES (1);"
        "INSERT INTO pg_wrapper_script_test (id) VALUES (2);");

    PgResult result = conn.execute("SELECT COUNT(*) FROM pg_wrapper_script_test");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "2");
}

// rowCount() на результате без строк — ветка, не задействованная ни одним
// существующим тестом (все они читают ровно одну строку).
TEST(PgWrapperIntegrationTest, RowCountIsZeroForEmptyResult){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    conn.execute("CREATE TEMPORARY TABLE pg_wrapper_empty_test (id INTEGER)");
    PgResult result = conn.execute("SELECT id FROM pg_wrapper_empty_test");

    EXPECT_EQ(result.rowCount(), 0);
}

// isNull() на непустом значении — обратная ветка от
// NullParameterReachesServerAsNull ниже, которая проверяет только "true".
TEST(PgWrapperIntegrationTest, IsNullReturnsFalseForNonNullValue){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    conn.execute("CREATE TEMPORARY TABLE pg_wrapper_not_null_test (price INTEGER)");
    conn.execute("INSERT INTO pg_wrapper_not_null_test (price) VALUES ($1)",
        {std::optional<std::string>("10")});

    PgResult result = conn.execute("SELECT price FROM pg_wrapper_not_null_test");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_FALSE(result.isNull(0, 0));
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

// columnCount() не был вызван ни одним тестом до этой правки — ни через
// прямой вызов, ни через какой-либо из репозиториев (все они читают
// значения по фиксированным индексам, не запрашивая число колонок).
TEST(PgWrapperIntegrationTest, ColumnCountReturnsNumberOfSelectedFields){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    PgResult result = conn.execute("SELECT 1, 2, 3");
    EXPECT_EQ(result.columnCount(), 3);
}

// execute()/executePrepared()/executeScript() на соединении, из которого уже
// переместили (conn_ == nullptr), обязаны бросать DatabaseError с понятным
// сообщением, а не разыменовывать пустой unique_ptr. Ни один из трёх методов
// не был проверен в этом состоянии ни одним тестом — MoveConstructor*-тесты
// выше проверяют только само перемещение, не последующее использование
// источника.
// Проверяет именно текст "moved from", а не только тип исключения: без
// собственной проверки conn_ на nullptr вызов PQexecParams(nullptr, ...)
// тоже возвращает NULL-результат и код всё равно бросает DatabaseError через
// более позднюю проверку "if(!result)" — тест на одном лишь типе исключения
// прошёл бы и без выделенной проверки "moved from" в начале execute().
TEST(PgWrapperIntegrationTest, ExecuteOnMovedFromConnectionThrows){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    PgConnection moved(std::move(*connOpt));
    try{
        connOpt->execute("SELECT 1");
        FAIL() << "Ожидалось исключение DatabaseError";
    }catch(const DatabaseError& e){
        EXPECT_NE(std::string(e.what()).find("moved from"), std::string::npos) << e.what();
    }
}

// См. пояснение у ExecuteOnMovedFromConnectionThrows выше — тот же приём
// нужен и здесь: PQprepare(nullptr, ...) тоже возвращает NULL и код бросает
// DatabaseError через отдельную более позднюю проверку, маскируя отсутствие
// выделенной проверки conn_ в начале executePrepared().
TEST(PgWrapperIntegrationTest, ExecutePreparedOnMovedFromConnectionThrows){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    PgConnection moved(std::move(*connOpt));
    try{
        connOpt->executePrepared("pg_wrapper_moved_from_test", "SELECT 1");
        FAIL() << "Ожидалось исключение DatabaseError";
    }catch(const DatabaseError& e){
        EXPECT_NE(std::string(e.what()).find("moved from"), std::string::npos) << e.what();
    }
}

// См. пояснение у ExecuteOnMovedFromConnectionThrows выше. executeScript()
// вообще не проверяет PQexec(...) на nullptr результат отдельно — без
// собственной проверки conn_ здесь PgResult(nullptr) бросит "libpq returned
// no result", тоже DatabaseError, но с другим текстом.
TEST(PgWrapperIntegrationTest, ExecuteScriptOnMovedFromConnectionThrows){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    PgConnection moved(std::move(*connOpt));
    try{
        connOpt->executeScript("SELECT 1");
        FAIL() << "Ожидалось исключение DatabaseError";
    }catch(const DatabaseError& e){
        EXPECT_NE(std::string(e.what()).find("moved from"), std::string::npos) << e.what();
    }
}

// isConnected() на соединении, из которого переместили, обязана вернуть
// false через короткое замыкание "conn_ &&..." — не обращаясь к PQstatus на
// пустом указателе. Все существующие тесты на isConnected() проверяют только
// живое и оборванное-сервером соединение, ни один — состояние после move.
TEST(PgWrapperIntegrationTest, IsConnectedReturnsFalseAfterMove){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    PgConnection moved(std::move(*connOpt));
    EXPECT_FALSE(connOpt->isConnected());
}

// Присваивание самому себе — "if(this == &other) return *this;" — не должно
// разрушать соединение. Обходной путь через указатель нужен, чтобы компилятор
// не подставил здесь статическую диагностику самоприсваивания и реально
// прогнал этот рантайм-путь.
TEST(PgWrapperIntegrationTest, MoveAssignmentToSelfIsNoOp){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    PgConnection* self = &conn;
    conn = std::move(*self);

    PgResult result = conn.execute("SELECT 1");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "1");
}

// MoveAssignmentWithActiveTransactionThrows (выше) проверяет только активную
// транзакцию на ИСТОЧНИКЕ присваивания. Условие в operator=() — "||" из двух
// независимых проверок, и ветка "активна транзакция на НАЗНАЧЕНИИ, источник
// свободен" ни разу не была пройдена ни одним тестом.
TEST(PgWrapperIntegrationTest, MoveAssignmentWithActiveTransactionOnDestinationThrows){
    auto destOpt = tryConnect();
    auto sourceOpt = tryConnect();
    if(!destOpt || !sourceOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    PgTransaction tx(*destOpt);
    EXPECT_THROW(*destOpt = std::move(*sourceOpt), DatabaseError);

    // Оба соединения остались рабочими — перемещения не произошло.
    PgResult result = sourceOpt->execute("SELECT 1");
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_EQ(result.getValue(0, 0), "1");
}

// ~PgTransaction() оборачивает ROLLBACK в try/catch(...) именно потому, что
// он может провалиться на уже неработоспособном соединении — деструктор не
// имеет права бросать. Этот путь не был пройден ни одним тестом: во всех
// остальных тестах соединение в момент разрушения PgTransaction остаётся
// рабочим. Ломает соединение тем же приёмом, что и
// IsConnectedReflectsActualConnectionState, но уже под открытой транзакцией.
TEST(PgWrapperIntegrationTest, TransactionDestructorSwallowsRollbackFailureOnBrokenConnection){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    {
        PgTransaction tx(conn);
        try{
            conn.execute("SELECT pg_terminate_backend(pg_backend_pid())");
        }catch(const DatabaseError&){
        }
        // Выход из области видимости здесь вызывает ~PgTransaction() на уже
        // оборванном соединении: ROLLBACK внутри обязан провалиться и быть
        // проглочен, а не вылететь из деструктора наружу.
    }

    // Сам факт, что мы дошли до этой строки, доказывает, что деструктор не
    // бросил исключение наружу. Он также обязан был декрементировать счётчик
    // активных транзакций несмотря на провал ROLLBACK — иначе следующее
    // перемещение соединения отказало бы с сообщением про "открытую
    // транзакцию", а не по настоящей причине (мёртвое соединение).
    PgConnection moved(std::move(conn));
    EXPECT_FALSE(moved.isConnected());
}
