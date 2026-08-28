#include<gtest/gtest.h>
#include<memory>
#include<optional>
#include<string>
#include<vector>
#include"command.hpp"
#include"command_processor.hpp"
#include"execution_result.hpp"
#include"order.hpp"
#include"pg_connection.hpp"
#include"pg_result.hpp"
#include"recovery_service.hpp"
#include"test_database.hpp"
#include"trade.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

// Задача 09: пять обязательных сценариев ТЗ (REQ-TEST-02..06) плюс
// интеграционные тесты, проверяющие связку MatchingEngine, репозиториев и
// реальной PostgreSQL целиком (REQ-TEST-07).
//
// Изоляция (критерий 7 задачи 09): каждый тест здесь идёт через
// CommandProcessor::process(), а тот открывает и коммитит СВОЮ собственную
// PgTransaction внутри PersistenceService — обернуть тест во внешнюю
// незакоммиченную транзакцию, как делают unit-тесты репозиториев, здесь
// нельзя (вложенный BEGIN даёт лишь предупреждение сервера, а внутренний
// COMMIT фиксирует и внешнюю транзакцию). Поэтому способ изоляции здесь
// другой: каждый тест очищает все три таблицы (trades, orders,
// processed_commands) на входе. Собственного диапазона order_id для этого
// недостаточно — recoverState() поднимает в книгу ВСЕ активные заявки
// таблицы orders, а не только заявки текущего теста, и любая посторонняя
// активная заявка (чужой тест, оставленный без очистки, или рабочие данные
// приложения) реально участвует в сопоставлении со встречной заявкой
// теста: чужая дешёвая SELL перехватит тестовую BUY, чужая дорогая BUY —
// тестовую SELL, при любом выборе цен диапазона. Диапазон order_id и
// префикс command_id у каждого теста здесь остаются (ниже, как константы
// kLow/kHigh и префиксы вида "idem-"), но уже не как способ изоляции, а
// чтобы тесты были читаемы и чтобы проверки вида "COUNT(*) ... WHERE
// buy_order_id BETWEEN ..." были строже безусловного счёта по таблице.
namespace{

// Полностью очищает три таблицы персистентности. Порядок важен:
// trades.buy_order_id/sell_order_id — внешние ключи на orders(order_id),
// поэтому сначала сделки, потом заявки, затем обработанные команды.
void cleanupAllTables(PgConnection& connection){
    connection.execute("DELETE FROM trades");
    connection.execute("DELETE FROM orders");
    connection.execute("DELETE FROM processed_commands");
}

}

// REQ-TEST-02: одна и та же команда с одним command_id отправляется дважды.
// После второго вызова состояние книги не изменилось, число сделок не
// изменилось, новых строк в trades нет.
//
// Сценарий сделан так, что "идемпотентность отсутствует" и "команда
// обработана дважды" дают РАЗЛИЧИМЫЙ результат: если бы кеш не сработал,
// второй вызов повторно сопоставил бы BUY с уже частично исполненной
// SELL, снял бы с неё ещё 4 единицы (осталось бы 2, а не 6) и добавил бы
// вторую строку в trades. Проверка "остаток заявки в книге" и "число строк
// в trades" — а не просто "команда не упала" — обязательна, иначе тест
// прошёл бы и на сломанной идемпотентности.
TEST(IntegrationTest, IdempotentCommandDoesNotDuplicateEffects){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr long long kLow = 901100000;
    constexpr long long kHigh = 901100099;
    constexpr int kSellId = 901100001;
    constexpr int kBuyId = 901100002;
    cleanupAllTables(conn);

    CommandProcessor processor;

    AddCommand sellSetup(kSellId, Side::Sell, 100, 10);
    sellSetup.commandId_ = "idem-setup-sell";
    processor.process(sellSetup, conn);

    AddCommand buyCommand(kBuyId, Side::Buy, 100, 4);
    buyCommand.commandId_ = "idem-buy-repeat";

    ExecutionResult first = processor.process(buyCommand, conn);
    ASSERT_EQ(first.trades.size(), 1u);
    EXPECT_EQ(first.trades[0].getBuyOrderId(), kBuyId);
    EXPECT_EQ(first.trades[0].getSellOrderId(), kSellId);
    EXPECT_EQ(first.trades[0].getQuantity(), 4);

    ASSERT_NE(processor.orderBook().findOrder(kSellId), nullptr);
    EXPECT_EQ(processor.orderBook().findOrder(kSellId)->getQuantity(), 6);
    // Полностью исполненная входящая заявка в книгу не попадает.
    EXPECT_EQ(processor.orderBook().findOrder(kBuyId), nullptr);

    PgResult tradesBefore = conn.execute(
        "SELECT COUNT(*) FROM trades WHERE buy_order_id = $1 AND sell_order_id = $2",
        {std::optional<std::string>(std::to_string(kBuyId)),
            std::optional<std::string>(std::to_string(kSellId))});
    ASSERT_EQ(tradesBefore.rowCount(), 1);
    EXPECT_EQ(tradesBefore.getValue(0, 0), "1");

    // Тот же command_id, второй раз — новый объект команды, но поля и
    // command_id идентичны первому вызову.
    AddCommand buyCommandRepeated(kBuyId, Side::Buy, 100, 4);
    buyCommandRepeated.commandId_ = "idem-buy-repeat";
    ExecutionResult second = processor.process(buyCommandRepeated, conn);

    ASSERT_EQ(second.trades.size(), 1u);
    EXPECT_EQ(second.trades[0].getBuyOrderId(), first.trades[0].getBuyOrderId());
    EXPECT_EQ(second.trades[0].getSellOrderId(), first.trades[0].getSellOrderId());
    EXPECT_EQ(second.trades[0].getQuantity(), first.trades[0].getQuantity());

    // Книга не изменилась.
    ASSERT_NE(processor.orderBook().findOrder(kSellId), nullptr);
    EXPECT_EQ(processor.orderBook().findOrder(kSellId)->getQuantity(), 6);
    EXPECT_EQ(processor.orderBook().findOrder(kBuyId), nullptr);

    // Число сделок не изменилось, новых строк в trades нет.
    PgResult tradesAfter = conn.execute(
        "SELECT COUNT(*) FROM trades WHERE buy_order_id = $1 AND sell_order_id = $2",
        {std::optional<std::string>(std::to_string(kBuyId)),
            std::optional<std::string>(std::to_string(kSellId))});
    ASSERT_EQ(tradesAfter.rowCount(), 1);
    EXPECT_EQ(tradesAfter.getValue(0, 0), "1");

    PgResult orderRow = conn.execute(
        "SELECT remaining_quantity FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kSellId))});
    ASSERT_EQ(orderRow.rowCount(), 1);
    EXPECT_EQ(orderRow.getValue(0, 0), "6");

    cleanupAllTables(conn);
}

// REQ-TEST-03: состояние из нескольких заявок на обеих сторонах, сохранить,
// создать новый экземпляр, восстановить — книга до и после эквивалентна.
// "Эквивалентна" проверяется по каждому полю каждой заявки (цена, остаток,
// исходное количество, статус, номер последовательности), а не только по
// факту их присутствия. Заявка buy100 частично исполняется до сохранения
// состояния: без этого остаток совпадал бы с исходным количеством, а статус
// был бы одинаков у всех трёх заявок (OPEN), и сравнение этих полей не
// отличило бы восстановленную книгу от книги с потерянным статусом или
// остатком.
//
// Сравнение идёт по конкретным идентификаторам заявок через findOrder(), а
// не по полным спискам buyOrders()/sellOrders(): recoverState() поднимает в
// книгу ВСЕ активные заявки таблицы orders, включая чужие (другие тесты,
// рабочие данные приложения), поэтому полный список после восстановления
// может быть длиннее, чем до него в рамках одного процесса.
TEST(IntegrationTest, RecoveryRestoresEquivalentOrderBook){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr long long kLow = 901200000;
    constexpr long long kHigh = 901200099;
    constexpr int kBuy100Id = 901200001;
    constexpr int kBuy99Id = 901200002;
    constexpr int kSell105Id = 901200003;
    constexpr int kPartialSellId = 901200004;
    cleanupAllTables(conn);

    CommandProcessor processor1;

    AddCommand buy100(kBuy100Id, Side::Buy, 100, 10);
    buy100.commandId_ = "rec-buy-100";
    processor1.process(buy100, conn);

    AddCommand buy99(kBuy99Id, Side::Buy, 99, 5);
    buy99.commandId_ = "rec-buy-99";
    processor1.process(buy99, conn);

    AddCommand sell105(kSell105Id, Side::Sell, 105, 3);
    sell105.commandId_ = "rec-sell-105";
    processor1.process(sell105, conn);

    // Частично исполняем buy100: встречная SELL по цене 100 пересекается
    // только с ним (buy99 стоит ниже, sell105 в книге не трогается),
    // поэтому его остаток и статус после этого отличаются от исходных
    // значений.
    AddCommand partialSell(kPartialSellId, Side::Sell, 100, 4);
    partialSell.commandId_ = "rec-partial-sell";
    ExecutionResult partialResult = processor1.process(partialSell, conn);
    ASSERT_EQ(partialResult.trades.size(), 1u);
    EXPECT_EQ(partialResult.trades[0].getBuyOrderId(), kBuy100Id);
    EXPECT_EQ(partialResult.trades[0].getQuantity(), 4);

    auto beforeBuy100 = processor1.orderBook().findOrder(kBuy100Id);
    auto beforeBuy99 = processor1.orderBook().findOrder(kBuy99Id);
    auto beforeSell105 = processor1.orderBook().findOrder(kSell105Id);
    ASSERT_NE(beforeBuy100, nullptr);
    ASSERT_NE(beforeBuy99, nullptr);
    ASSERT_NE(beforeSell105, nullptr);
    // Остаток buy100 отличается от исходного количества, статус — от OPEN.
    EXPECT_EQ(beforeBuy100->getQuantity(), 6);
    EXPECT_EQ(beforeBuy100->getInitialQuantity(), 10);
    EXPECT_EQ(beforeBuy100->getStatus(), OrderStatus::PartiallyFilled);

    // "Новый экземпляр" эмулируется новым CommandProcessor и явным вызовом
    // recoverState (docs/tasks/task-09.md, "Доменные правила", п.3) —
    // соединение переиспользуется, восстановление читает уже закоммиченное.
    CommandProcessor processor2;
    recoverState(conn, processor2);

    auto afterBuy100 = processor2.orderBook().findOrder(kBuy100Id);
    auto afterBuy99 = processor2.orderBook().findOrder(kBuy99Id);
    auto afterSell105 = processor2.orderBook().findOrder(kSell105Id);

    for(const auto& pair : std::vector<std::pair<std::shared_ptr<Order>, std::shared_ptr<Order>>>{
            {beforeBuy100, afterBuy100}, {beforeBuy99, afterBuy99},
            {beforeSell105, afterSell105}}){
        const auto& before = pair.first;
        const auto& after = pair.second;
        SCOPED_TRACE("order id=" + std::to_string(before->getId()));
        ASSERT_NE(after, nullptr);
        EXPECT_EQ(after->getId(), before->getId());
        EXPECT_EQ(after->getSide(), before->getSide());
        EXPECT_EQ(after->getPrice(), before->getPrice());
        EXPECT_EQ(after->getInitialQuantity(), before->getInitialQuantity());
        EXPECT_EQ(after->getQuantity(), before->getQuantity());
        EXPECT_EQ(after->getStatus(), before->getStatus());
        EXPECT_EQ(after->getSequenceNumber(), before->getSequenceNumber());
    }

    cleanupAllTables(conn);
}

// REQ-TEST-04: три BUY по одной цене с разным количеством, перезапуск,
// затем встречный SELL — исполнение обязано пойти в исходном порядке
// поступления.
//
// Идентификаторы заявок намеренно НЕ совпадают с порядком их поступления
// (первой приходит kFirstId, второй — kSecondId, третьей — kThirdId, и их
// числовые значения идут не по возрастанию; docs/tasks/task-09.md,
// "Доменные правила", п.5): если бы восстановление сортировало активные
// заявки по order_id, а не по sequence_number, тест бы это поймал, а тест,
// где id совпадают с порядком поступления, — нет.
//
// Одной сортировки по order_id недостаточно, чтобы поймать пропажу
// "ORDER BY sequence_number ASC" целиком: если заявки только вставлены и ни
// разу не обновлены, физический порядок строк в куче совпадает с порядком
// вставки, и seq-scan без ORDER BY случайно возвращает тот же порядок, что
// и сортировка по sequence_number. Поэтому самая ранняя заявка (kFirstId)
// частично исполняется ДО "перезапуска": UPDATE переписывает её строку и
// физически переносит её в куче, а sequence_number при этом не меняется —
// физический порядок строк и порядок поступления расходятся, и только
// ORDER BY sequence_number ASC возвращает их в верном порядке.
//
// Количества выбраны так, чтобы порядок сделок был виден по количеству в
// каждой сделке, а не только по факту "сделки есть".
TEST(IntegrationTest, TimePriorityPreservedAfterRestart){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr long long kLow = 901300000;
    constexpr long long kHigh = 901300099;
    constexpr int kFirstId = 901300030;   // приходит первой
    constexpr int kSecondId = 901300010;  // приходит второй
    constexpr int kThirdId = 901300020;   // приходит третьей
    constexpr int kPartialSellId = 901300031;
    constexpr int kFinalSellId = 901300099;
    cleanupAllTables(conn);

    CommandProcessor processor1;

    AddCommand first(kFirstId, Side::Buy, 100, 2);
    first.commandId_ = "tp-first";
    processor1.process(first, conn);

    AddCommand second(kSecondId, Side::Buy, 100, 3);
    second.commandId_ = "tp-second";
    processor1.process(second, conn);

    AddCommand third(kThirdId, Side::Buy, 100, 4);
    third.commandId_ = "tp-third";
    processor1.process(third, conn);

    // Частичное исполнение самой ранней заявки (kFirstId) на 1 единицу:
    // UPDATE переписывает её строку в orders, sequence_number не трогает.
    AddCommand partial(kPartialSellId, Side::Sell, 100, 1);
    partial.commandId_ = "tp-partial";
    ExecutionResult partialResult = processor1.process(partial, conn);
    ASSERT_EQ(partialResult.trades.size(), 1u);
    EXPECT_EQ(partialResult.trades[0].getBuyOrderId(), kFirstId);
    EXPECT_EQ(partialResult.trades[0].getQuantity(), 1);
    ASSERT_NE(processor1.orderBook().findOrder(kFirstId), nullptr);
    EXPECT_EQ(processor1.orderBook().findOrder(kFirstId)->getQuantity(), 1);

    CommandProcessor processor2;
    recoverState(conn, processor2);

    // Встречная SELL ровно на объём kSecondId (3) плюс остаток kFirstId (1):
    // если приоритет после восстановления сохранился верно, сделка сначала
    // добьёт kFirstId, потом целиком заберёт kSecondId, а kThirdId останется
    // нетронутым в книге.
    AddCommand sell(kFinalSellId, Side::Sell, 100, 4);
    sell.commandId_ = "tp-sell";
    ExecutionResult sellResult = processor2.process(sell, conn);

    ASSERT_EQ(sellResult.trades.size(), 2u);
    EXPECT_EQ(sellResult.trades[0].getBuyOrderId(), kFirstId);
    EXPECT_EQ(sellResult.trades[0].getSellOrderId(), kFinalSellId);
    EXPECT_EQ(sellResult.trades[0].getQuantity(), 1);
    EXPECT_EQ(sellResult.trades[1].getBuyOrderId(), kSecondId);
    EXPECT_EQ(sellResult.trades[1].getSellOrderId(), kFinalSellId);
    EXPECT_EQ(sellResult.trades[1].getQuantity(), 3);

    EXPECT_EQ(processor2.orderBook().findOrder(kFirstId), nullptr);
    EXPECT_EQ(processor2.orderBook().findOrder(kSecondId), nullptr);
    auto untouched = processor2.orderBook().findOrder(kThirdId);
    ASSERT_NE(untouched, nullptr);
    EXPECT_EQ(untouched->getQuantity(), 4);

    cleanupAllTables(conn);
}

// REQ-TEST-05: несколько сделок совершены — они присутствуют в БД
// (проверка на уровне строк таблицы trades, а не через API репозитория,
// которым он же и наполнялся).
TEST(IntegrationTest, TradesArePersistedInDatabase){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr long long kLow = 901400000;
    constexpr long long kHigh = 901400099;
    constexpr int kSellId = 901400001;
    constexpr int kBuyAId = 901400002;
    constexpr int kBuyBId = 901400003;
    cleanupAllTables(conn);

    CommandProcessor processor;

    AddCommand sell(kSellId, Side::Sell, 100, 10);
    sell.commandId_ = "persist-sell";
    processor.process(sell, conn);

    AddCommand buyA(kBuyAId, Side::Buy, 100, 4);
    buyA.commandId_ = "persist-buy-a";
    processor.process(buyA, conn);

    AddCommand buyB(kBuyBId, Side::Buy, 100, 6);
    buyB.commandId_ = "persist-buy-b";
    processor.process(buyB, conn);

    PgResult rows = conn.execute(
        "SELECT buy_order_id, sell_order_id, price, quantity FROM trades "
        "WHERE sell_order_id = $1 ORDER BY trade_id",
        {std::optional<std::string>(std::to_string(kSellId))});
    ASSERT_EQ(rows.rowCount(), 2);

    EXPECT_EQ(rows.getValue(0, 0), std::to_string(kBuyAId));
    EXPECT_EQ(rows.getValue(0, 1), std::to_string(kSellId));
    EXPECT_EQ(rows.getValue(0, 2), "100");
    EXPECT_EQ(rows.getValue(0, 3), "4");

    EXPECT_EQ(rows.getValue(1, 0), std::to_string(kBuyBId));
    EXPECT_EQ(rows.getValue(1, 1), std::to_string(kSellId));
    EXPECT_EQ(rows.getValue(1, 2), "100");
    EXPECT_EQ(rows.getValue(1, 3), "6");

    cleanupAllTables(conn);
}

// REQ-TEST-06: заявка создана, приложение перезапущено, отмена работает.
TEST(IntegrationTest, CancelWorksAfterRestart){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr long long kLow = 901500000;
    constexpr long long kHigh = 901500099;
    constexpr int kAddId = 901500001;
    cleanupAllTables(conn);

    CommandProcessor processor1;

    AddCommand add(kAddId, Side::Buy, 100, 10);
    add.commandId_ = "cancel-add";
    processor1.process(add, conn);

    CommandProcessor processor2;
    recoverState(conn, processor2);

    ASSERT_NE(processor2.orderBook().findOrder(kAddId), nullptr);
    EXPECT_EQ(processor2.orderBook().findOrder(kAddId)->getQuantity(), 10);

    CancelCommand cancel(kAddId);
    cancel.commandId_ = "cancel-cancel";
    processor2.process(cancel, conn);

    EXPECT_EQ(processor2.orderBook().findOrder(kAddId), nullptr);

    PgResult row = conn.execute(
        "SELECT status FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kAddId))});
    ASSERT_EQ(row.rowCount(), 1);
    EXPECT_EQ(row.getValue(0, 0), "CANCELLED");

    cleanupAllTables(conn);
}

// REQ-TEST-07: интеграционный тест, дополняющий пять обязательных —
// проверяет связку MatchingEngine + OrderRepository + PostgreSQL на менее
// очевидном сценарии: MODIFY реализован как remove + re-add ("Ключевые правила домена"), поэтому изменённая заявка теряет временной
// приоритет. Тест проверяет, что это поведение переживает перезапуск: после
// восстановления книги приоритет отражает НОВЫЙ номер последовательности,
// присвоенный при MODIFY, а не исходный порядок поступления заявок.
TEST(IntegrationTest, ModifyResetsTimePriorityAcrossRestart){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr long long kLow = 901600000;
    constexpr long long kHigh = 901600099;
    constexpr int kTenId = 901600010;
    constexpr int kTwentyId = 901600020;
    constexpr int kSellId = 901600099;
    cleanupAllTables(conn);

    CommandProcessor processor1;

    // kTenId приходит первым...
    AddCommand orderTen(kTenId, Side::Buy, 100, 5);
    orderTen.commandId_ = "modify-add-10";
    processor1.process(orderTen, conn);

    // ...kTwentyId приходит вторым.
    AddCommand orderTwenty(kTwentyId, Side::Buy, 100, 5);
    orderTwenty.commandId_ = "modify-add-20";
    processor1.process(orderTwenty, conn);

    // MODIFY меняет остаток kTenId и, по конструкции движка, пересоздаёт
    // заявку с новым sequence_number — kTenId теряет приоритет перед
    // kTwentyId, хотя пришёл раньше.
    ModifyCommand modifyTen(kTenId, 100, 3);
    modifyTen.commandId_ = "modify-modify-10";
    processor1.process(modifyTen, conn);

    ASSERT_NE(processor1.orderBook().findOrder(kTenId), nullptr);
    EXPECT_EQ(processor1.orderBook().findOrder(kTenId)->getQuantity(), 3);

    CommandProcessor processor2;
    recoverState(conn, processor2);

    // Встречная SELL ровно на объём kTwentyId (5): если приоритет после
    // восстановления сохранился верно (kTwentyId впереди kTenId), сделка
    // пойдёт против kTwentyId целиком, а kTenId останется нетронутым в
    // книге.
    AddCommand sell(kSellId, Side::Sell, 100, 5);
    sell.commandId_ = "modify-sell";
    ExecutionResult sellResult = processor2.process(sell, conn);

    ASSERT_EQ(sellResult.trades.size(), 1u);
    EXPECT_EQ(sellResult.trades[0].getBuyOrderId(), kTwentyId);
    EXPECT_EQ(sellResult.trades[0].getSellOrderId(), kSellId);
    EXPECT_EQ(sellResult.trades[0].getQuantity(), 5);

    EXPECT_EQ(processor2.orderBook().findOrder(kTwentyId), nullptr);
    auto orderTenAfter = processor2.orderBook().findOrder(kTenId);
    ASSERT_NE(orderTenAfter, nullptr);
    EXPECT_EQ(orderTenAfter->getQuantity(), 3);

    cleanupAllTables(conn);
}
