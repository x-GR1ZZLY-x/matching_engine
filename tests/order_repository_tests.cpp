#include<gtest/gtest.h>
#include<cstdlib>
#include<memory>
#include<optional>
#include<string>
#include<vector>
#include"exceptions.hpp"
#include"execution_result.hpp"
#include"order.hpp"
#include"order_repository.hpp"
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

// Идентификаторы заявок и номера последовательности в тестах заведомо не
// пересекаются с рабочими данными (9-значные номера с общим префиксом
// 900000...). SequenceGenerator выдаёт номера начиная с единицы, поэтому
// после первого реального прогона приложения в таблице появятся строки
// с sequence_number = 1, 2, 3... — малые значения здесь недопустимы.
OrderChange makeChange(int id, Side side, std::optional<int> price,
    int initialQuantity, int remainingQuantity, OrderStatus status,
    long long sequenceNumber){
    return OrderChange{id, side, price, initialQuantity, remainingQuantity,
        status, sequenceNumber};
}

}

// Критерии 7 и 10: пустая цена рыночной заявки должна читаться как NULL, а
// не как пустая строка — различить их можно только через isNull().
TEST(OrderRepositoryTest, MarketOrderPriceIsStoredAndReadAsNullNotEmptyString){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository repo;
    // Рыночная заявка: цена nullopt, статус — отменена (частично исполнена,
    // непокрытый остаток отброшен, в книге такая заявка не остаётся).
    repo.save(conn, makeChange(900000101, Side::Buy, std::nullopt, 100, 60,
        OrderStatus::Cancelled, 900000001));

    // Проверка на уровне строки таблицы: у OrderRepository нет метода
    // чтения, возвращающего заявку с пустой ценой (loadActive рыночные
    // не выбирает).
    PgResult result = conn.execute(
        "SELECT price FROM orders WHERE order_id = $1",
        {std::optional<std::string>("900000101")});
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_TRUE(result.isNull(0, 0));
    // getValue возвращает пустую строку и для NULL, и для пустого значения —
    // сам по себе этот факт не отличает одно от другого.
    EXPECT_EQ(result.getValue(0, 0), "");
}

TEST(OrderRepositoryTest, LimitOrderPriceIsNotNull){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository repo;
    repo.save(conn, makeChange(900000102, Side::Sell, 150, 10, 10,
        OrderStatus::Open, 900000002));

    PgResult result = conn.execute(
        "SELECT price FROM orders WHERE order_id = $1",
        {std::optional<std::string>("900000102")});
    ASSERT_EQ(result.rowCount(), 1);
    EXPECT_FALSE(result.isNull(0, 0));
    EXPECT_EQ(result.getValue(0, 0), "150");
}

// Критерий 5: загрузка активных заявок возвращает их строго в порядке
// возрастания номера последовательности — вставлены в обратном порядке.
TEST(OrderRepositoryTest, LoadActiveReturnsOrdersOrderedBySequenceNumber){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository repo;
    repo.save(conn, makeChange(900000203, Side::Buy, 100, 5, 5, OrderStatus::Open, 900000030));
    repo.save(conn, makeChange(900000201, Side::Buy, 100, 5, 5, OrderStatus::Open, 900000010));
    repo.save(conn, makeChange(900000202, Side::Buy, 100, 5, 5, OrderStatus::Open, 900000020));

    auto orders = repo.loadActive(conn);

    // Отбираем только тестовые заявки: относительный порядок среди них
    // сохраняется независимо от того, что ещё лежит в таблице.
    std::vector<std::shared_ptr<Order>> testOrders;
    for(const auto& order : orders){
        if(order->getId() == 900000201 || order->getId() == 900000202 ||
            order->getId() == 900000203){
            testOrders.push_back(order);
        }
    }

    ASSERT_EQ(testOrders.size(), 3u);
    EXPECT_EQ(testOrders[0]->getId(), 900000201);
    EXPECT_EQ(testOrders[1]->getId(), 900000202);
    EXPECT_EQ(testOrders[2]->getId(), 900000203);
}

// Критерий 6: загруженная заявка сохраняет записанные номер
// последовательности, исходное количество и статус — они не пересоздаются.
TEST(OrderRepositoryTest, LoadActivePreservesSequenceNumberInitialQuantityAndStatus){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository repo;
    repo.save(conn, makeChange(900000301, Side::Sell, 200, 100, 35,
        OrderStatus::PartiallyFilled, 900000555));

    auto orders = repo.loadActive(conn);
    std::shared_ptr<Order> found;
    for(const auto& order : orders){
        if(order->getId() == 900000301){
            found = order;
        }
    }

    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->getSequenceNumber(), 900000555);
    EXPECT_EQ(found->getInitialQuantity(), 100);
    EXPECT_EQ(found->getQuantity(), 35);
    EXPECT_EQ(found->getStatus(), OrderStatus::PartiallyFilled);
}

// FILLED и CANCELLED в загрузку активных попадать не должны.
TEST(OrderRepositoryTest, LoadActiveExcludesFilledAndCancelledOrders){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository repo;
    repo.save(conn, makeChange(900000401, Side::Buy, 100, 10, 0, OrderStatus::Filled, 900000601));
    repo.save(conn, makeChange(900000402, Side::Buy, 100, 10, 4, OrderStatus::Cancelled, 900000602));

    auto orders = repo.loadActive(conn);
    for(const auto& order : orders){
        EXPECT_NE(order->getId(), 900000401);
        EXPECT_NE(order->getId(), 900000402);
    }
}

// Пункт 3a задачи 06: у рыночной заявки (пустая цена) активных статусов
// быть не может — save() должен отказать fail-fast, а не сохранить строку,
// на которой позже упадёт loadActive() при рестарте.
TEST(OrderRepositoryTest, SaveRejectsActiveMarketOrder){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository repo;
    EXPECT_THROW(repo.save(conn, makeChange(900000501, Side::Buy, std::nullopt, 10, 10,
        OrderStatus::Open, 900000701)), MatchingEngineError);
    EXPECT_THROW(repo.save(conn, makeChange(900000502, Side::Buy, std::nullopt, 10, 5,
        OrderStatus::PartiallyFilled, 900000702)), MatchingEngineError);
}

// Задача 08, критерий 4: maxSequenceNumber() обязан видеть исполненную
// заявку с номером выше, чем у любой активной, — loadActive() её не вернёт
// (фильтр по статусу), поэтому взять максимум только по её результату было
// бы ровно той ошибкой, о которой предупреждает критерий.
TEST(OrderRepositoryTest, MaxSequenceNumberSeesFilledOrderAboveAnyActiveOne){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    PgTransaction tx(conn);

    OrderRepository repo;
    repo.save(conn, makeChange(900000901, Side::Buy, 100, 10, 10,
        OrderStatus::Open, 900000910));
    repo.save(conn, makeChange(900000902, Side::Sell, 100, 10, 0,
        OrderStatus::Filled, 900000999));

    // Не EXPECT_EQ: тест проверяет, что максимум виден по всей таблице (а не
    // только по активным заявкам), а не что в рабочей базе больше нет других
    // строк с большим номером. Если бы maxSequenceNumber() ошибочно смотрел
    // только на активные заявки, вернулось бы меньшее значение и это всё
    // равно упало бы.
    EXPECT_GE(repo.maxSequenceNumber(conn), 900000999);
}
