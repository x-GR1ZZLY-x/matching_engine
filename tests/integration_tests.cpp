#include<gtest/gtest.h>
#include<filesystem>
#include<fstream>
#include<functional>
#include<memory>
#include<nlohmann/json.hpp>
#include<optional>
#include<string>
#include<vector>
#include"application.hpp"
#include"command.hpp"
#include"command_processor.hpp"
#include"exceptions.hpp"
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

// Пять обязательных сценариев ТЗ (REQ-TEST-02..06) плюс
// интеграционные тесты, проверяющие связку MatchingEngine, репозиториев и
// реальной PostgreSQL целиком (REQ-TEST-07).
//
// Изоляция: каждый тест здесь идёт через
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

// Запускает Application::run(argv) с рабочим каталогом процесса,
// временно переставленным в корень репозитория, — так же, как это уже
// делает ApplicationRunReplaySkipsBadLineAndPersistsValidOnes ниже:
// applySchema() внутри Application::run получает schemaDir из конфигурации
// как относительный или абсолютный путь, а тесты типовых сценариев
// используют тот же конфиг, что и приложение целиком (config/config.json),
// где schema_dir прописан относительным ("database"). Возврат к прежнему
// каталогу гарантирован RAII-объектом ниже даже при исключении.
class ScopedWorkingDirectory{
public:
    ScopedWorkingDirectory() : previous_(std::filesystem::current_path()){
        std::filesystem::current_path(MATCHING_ENGINE_SOURCE_DIR);
    }
    ~ScopedWorkingDirectory(){
        std::filesystem::current_path(previous_);
    }
    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;

private:
    std::filesystem::path previous_;
};

int runApplication(std::vector<std::string> args){
    ScopedWorkingDirectory cwd;

    std::vector<char*> argv{const_cast<char*>("matching_engine")};
    for(auto& arg : args){
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    Application app;
    return app.run(static_cast<int>(argv.size()), argv.data());
}

// Копирует реальный config/config.json, подменяя одно поле (заданное
// патчем), и возвращает путь к временному файлу — для сценариев,
// требующих отличную от рабочей конфигурацию (битый schema_dir,
// недоступный порт БД), но не желающих задавать все поля с нуля и тем
// самым расходиться с реальными значениями хоста/имени БД/пользователя.
std::string writePatchedConfig(const std::string& suffix,
    const std::function<void(nlohmann::json&)>& patch){

    std::ifstream real(matching_engine::test::configPath());
    nlohmann::json root;
    real >> root;
    patch(root);

    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("matching_engine_config_" + suffix + ".json");
    std::ofstream out(path);
    out << root.dump();
    return path.string();
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
    // recoverState — соединение переиспользуется, восстановление читает уже закоммиченное.
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
// числовые значения идут не по возрастанию): если бы восстановление сортировало активные
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
// очевидном сценарии: MODIFY реализован как remove + re-add ("Ключевые
// правила домена"), поэтому изменённая заявка теряет временной
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

// Application::run --replay
// печатает сводку, некорректная строка не прерывает прогон, режим идёт
// через тот же путь сохранения) до сих пор проверялись только вручную.
// Единственный интеграционный тест на весь Application::run --replay:
// файл из трёх строк (валидная ADD, битый JSON, валидная ADD) обязан дать
// код возврата 0 и ровно две строки в processed_commands.
TEST(IntegrationTest, ApplicationRunReplaySkipsBadLineAndPersistsValidOnes){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    cleanupAllTables(conn);

    constexpr int kFirstId = 901700001;
    constexpr int kSecondId = 901700002;

    const std::filesystem::path replayPath = std::filesystem::temp_directory_path() /
        "matching_engine_replay_app_test.jsonl";
    {
        std::ofstream out(replayPath);
        out << "{\"type\":\"ADD\",\"id\":" << kFirstId
            << ",\"side\":\"BUY\",\"price\":100,\"quantity\":5,"
               "\"command_id\":\"replay-app-1\"}\n";
        out << "{this is not valid json\n";
        out << "{\"type\":\"ADD\",\"id\":" << kSecondId
            << ",\"side\":\"SELL\",\"price\":200,\"quantity\":3,"
               "\"command_id\":\"replay-app-2\"}\n";
    }

    // Application::run применяет схему из ОТНОСИТЕЛЬНОГО пути "database"
    // (src/application.cpp, kSchemaDir), а ctest запускает тесты с рабочим
    // каталогом build/ — тот же приём, что SchemaTest применяет через
    // MATCHING_ENGINE_SOURCE_DIR, только здесь нужно временно сменить
    // текущий каталог процесса, а не собрать абсолютный путь строкой (сам
    // Application::run пути не параметризует). Возврат к прежнему каталогу
    // обязан произойти и на пути исключения, иначе следующий тест в этом
    // процессе побежит из чужого каталога.
    const std::filesystem::path previousCwd = std::filesystem::current_path();
    std::filesystem::current_path(MATCHING_ENGINE_SOURCE_DIR);

    int exitCode = 1;
    try{
        const std::string configPath = matching_engine::test::configPath();
        const std::string replayPathStr = replayPath.string();
        std::vector<char*> argv{
            const_cast<char*>("matching_engine"),
            const_cast<char*>("--config"),
            const_cast<char*>(configPath.c_str()),
            const_cast<char*>("--replay"),
            const_cast<char*>(replayPathStr.c_str())};

        Application app;
        exitCode = app.run(static_cast<int>(argv.size()), argv.data());
    } catch(...){
        std::filesystem::current_path(previousCwd);
        std::filesystem::remove(replayPath);
        throw;
    }
    std::filesystem::current_path(previousCwd);
    std::filesystem::remove(replayPath);

    EXPECT_EQ(exitCode, 0);

    PgResult count = conn.execute(
        "SELECT COUNT(*) FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("replay-app-1"), std::optional<std::string>("replay-app-2")});
    ASSERT_EQ(count.rowCount(), 1);
    EXPECT_EQ(count.getValue(0, 0), "2");

    cleanupAllTables(conn);
}

// Application::run: файл конфигурации не найден — ConfigError перехвачен до
// всякого обращения к БД (REQ-RAII-09 не затрагивается: соединение ещё не
// открыто). Не требует БД вовсе.
TEST(IntegrationTest, ApplicationRunWithMissingConfigFileReturnsErrorExitCode){
    testing::internal::CaptureStderr();
    const int exitCode = runApplication({"--config", "/nonexistent/matching_engine_config.json",
        "{\"commands\":[]}"});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("ERROR:"), std::string::npos);
}

// Application::run: конфигурация читается, но порт БД недоступен —
// describeConnectionFailure() формирует сообщение, PgConnection бросает
// MatchingEngineError до applySchema/recoverState. Порт 1 выбран как
// заведомо не слушающий ни один сервер PostgreSQL в тестовом окружении.
TEST(IntegrationTest, ApplicationRunWithUnreachableDatabasePortReturnsErrorExitCode){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    const std::string configPath = writePatchedConfig("bad_port",
        [](nlohmann::json& root){ root["database"]["port"] = 1; });

    testing::internal::CaptureStderr();
    const int exitCode = runApplication({"--config", configPath, "{\"commands\":[]}"});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("Cannot connect to the database"), std::string::npos);

    std::filesystem::remove(configPath);
}

// Application::run: соединение открыто, но schema_dir указывает на
// несуществующий каталог — describeSchemaFailure() перехватывает
// MatchingEngineError из applySchema до recoverState.
TEST(IntegrationTest, ApplicationRunWithBadSchemaDirReturnsErrorExitCode){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    const std::string configPath = writePatchedConfig("bad_schema",
        [](nlohmann::json& root){
            root["database"]["schema_dir"] = "/nonexistent/matching_engine_schema_dir";
        });

    testing::internal::CaptureStderr();
    const int exitCode = runApplication({"--config", configPath, "{\"commands\":[]}"});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("Failed to apply the database schema"), std::string::npos);

    std::filesystem::remove(configPath);
}

// Application::run: полный обычный старт (конфиг, соединение, схема,
// восстановление) доходит до разбора позиционного JSON-аргумента, а тот
// синтаксически невалиден.
TEST(IntegrationTest, ApplicationRunWithInvalidJsonArgReturnsErrorExitCode){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    testing::internal::CaptureStderr();
    const int exitCode = runApplication({"{not valid json"});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("Invalid JSON"), std::string::npos);
}

// Позиционный аргумент — синтаксически валидный JSON, но не объект с полем
// "commands" (здесь — массив верхнего уровня). Эта проверка стоит раньше
// входа в цикл именно для того, чтобы не поймать вместо этого
// нативное исключение nlohmann::json::type_error.
TEST(IntegrationTest, ApplicationRunWithTopLevelJsonArrayReturnsErrorExitCode){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    testing::internal::CaptureStderr();
    const int exitCode = runApplication({"[]"});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("must be an object"), std::string::npos);
}

// PRINT в позиционном режиме (не --replay): processCommand() возвращает
// Status::Printed, run() не прерывается и печатает книгу заявок в stdout.
TEST(IntegrationTest, ApplicationRunWithPrintCommandPrintsOrderBookAndSucceeds){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    testing::internal::CaptureStdout();
    const int exitCode = runApplication({"{\"commands\":[{\"type\":\"PRINT\"}]}"});
    const std::string stdoutOutput = testing::internal::GetCapturedStdout();

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(stdoutOutput.find("ORDER BOOK"), std::string::npos);
}

// Ошибочная команда (отсутствует обязательный command_id) не прерывает
// обработку остальных команд массива ("Обработка ошибок" в CLAUDE.md) —
// после неё валидная ADD всё равно применяется и сохраняется.
TEST(IntegrationTest, ApplicationRunContinuesAfterFailingCommandInArray){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kOrderId = 901900001;
    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("run-continues-after-failure")});

    const std::string commands = "{\"commands\":["
        "{\"type\":\"ADD\",\"id\":901900099,\"side\":\"BUY\",\"price\":100,\"quantity\":1},"
        "{\"type\":\"ADD\",\"id\":" + std::to_string(kOrderId) + ",\"side\":\"BUY\",\"price\":100,"
        "\"quantity\":5,\"command_id\":\"run-continues-after-failure\"}]}";

    testing::internal::CaptureStderr();
    const int exitCode = runApplication({commands});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(stderrOutput.find("ERROR:"), std::string::npos);

    PgResult row = conn.execute("SELECT remaining_quantity FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    ASSERT_EQ(row.rowCount(), 1);
    EXPECT_EQ(row.getValue(0, 0), "5");

    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("run-continues-after-failure")});
}

// --replay с несуществующим файлом: runReplay() возвращает false до всякого
// чтения строк, run() транслирует это в код возврата 1.
TEST(IntegrationTest, ApplicationRunReplayWithMissingFileReturnsErrorExitCode){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    testing::internal::CaptureStderr();
    const int exitCode = runApplication({"--replay", "/nonexistent/matching_engine_replay.jsonl"});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("Cannot open replay file"), std::string::npos);
}

// --replay --batch: команды копятся в буфере CommandProcessor и сбрасываются
// один раз по достижении конца файла (ветка "if(batch){ flushBatch }" после
// цикла в runReplay) — до этой правки не проверялось ни разу.
TEST(IntegrationTest, ApplicationRunReplayWithBatchFlagPersistsViaFlushBatch){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kSellId = 902000001;
    constexpr int kBuyId = 902000002;
    conn.execute("DELETE FROM trades WHERE buy_order_id = $1 OR sell_order_id = $1",
        {std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM orders WHERE order_id = $1 OR order_id = $2",
        {std::optional<std::string>(std::to_string(kSellId)),
            std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("replay-batch-sell"),
            std::optional<std::string>("replay-batch-buy")});

    const std::filesystem::path replayPath = std::filesystem::temp_directory_path() /
        "matching_engine_replay_batch_test.jsonl";
    {
        std::ofstream out(replayPath);
        out << "{\"type\":\"ADD\",\"id\":" << kSellId
            << ",\"side\":\"SELL\",\"price\":100,\"quantity\":10,"
               "\"command_id\":\"replay-batch-sell\"}\n";
        out << "{\"type\":\"ADD\",\"id\":" << kBuyId
            << ",\"side\":\"BUY\",\"price\":100,\"quantity\":4,"
               "\"command_id\":\"replay-batch-buy\"}\n";
    }

    testing::internal::CaptureStdout();
    const int exitCode = runApplication({"--replay", replayPath.string(), "--batch"});
    const std::string stdoutOutput = testing::internal::GetCapturedStdout();
    std::filesystem::remove(replayPath);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(stdoutOutput.find("processed=2"), std::string::npos);
    EXPECT_NE(stdoutOutput.find("trades=1"), std::string::npos);

    PgResult count = conn.execute(
        "SELECT COUNT(*) FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("replay-batch-sell"),
            std::optional<std::string>("replay-batch-buy")});
    ASSERT_EQ(count.rowCount(), 1);
    EXPECT_EQ(count.getValue(0, 0), "2");

    conn.execute("DELETE FROM trades WHERE buy_order_id = $1 OR sell_order_id = $1",
        {std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM orders WHERE order_id = $1 OR order_id = $2",
        {std::optional<std::string>(std::to_string(kSellId)),
            std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("replay-batch-sell"),
            std::optional<std::string>("replay-batch-buy")});
}

// Тот же command_id дважды в одном файле --replay: второй раз обслуживается
// из кеша идемпотентности (Status::Duplicate), попадает в duplicates, а не
// в processed/trades сводки — CommandOutcome::Status::Duplicate до этой
// правки не проверялся через Application::run вовсе.
TEST(IntegrationTest, ApplicationRunReplayWithDuplicateCommandIdReportsDuplicate){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kOrderId = 902100001;
    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("replay-duplicate")});

    const std::filesystem::path replayPath = std::filesystem::temp_directory_path() /
        "matching_engine_replay_duplicate_test.jsonl";
    {
        std::ofstream out(replayPath);
        out << "{\"type\":\"ADD\",\"id\":" << kOrderId
            << ",\"side\":\"BUY\",\"price\":100,\"quantity\":5,"
               "\"command_id\":\"replay-duplicate\"}\n";
        out << "{\"type\":\"ADD\",\"id\":" << kOrderId
            << ",\"side\":\"BUY\",\"price\":100,\"quantity\":5,"
               "\"command_id\":\"replay-duplicate\"}\n";
    }

    testing::internal::CaptureStdout();
    const int exitCode = runApplication({"--replay", replayPath.string()});
    const std::string stdoutOutput = testing::internal::GetCapturedStdout();
    std::filesystem::remove(replayPath);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(stdoutOutput.find("processed=1"), std::string::npos);
    EXPECT_NE(stdoutOutput.find("duplicates=1"), std::string::npos);

    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>("replay-duplicate")});
}

// Пустой файл --replay: цикл getline не выполняет ни одной итерации,
// file.bad() ложно, сводка печатается с нулевыми счётчиками — отдельная
// ветка от файла с содержимым.
TEST(IntegrationTest, ApplicationRunReplayWithEmptyFileSucceedsWithZeroCounters){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }

    const std::filesystem::path replayPath = std::filesystem::temp_directory_path() /
        "matching_engine_replay_empty_test.jsonl";
    { std::ofstream out(replayPath); }

    testing::internal::CaptureStdout();
    const int exitCode = runApplication({"--replay", replayPath.string()});
    const std::string stdoutOutput = testing::internal::GetCapturedStdout();
    std::filesystem::remove(replayPath);

    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(stdoutOutput.find("processed=0"), std::string::npos);
}

// Application::run: ни --replay, ни позиционного JSON-аргумента —
// parseArgs() возвращает errorMessage == kUsage, и run() обязан различить
// этот случай от прочих ошибок разбора: usage печатается через
// printer_.printUsage (без префикса ERROR:), а не через printError
// (с префиксом). Не требует БД: до открытия соединения дело не доходит.
TEST(IntegrationTest, ApplicationRunWithNoArgsPrintsUsage){
    testing::internal::CaptureStderr();
    const int exitCode = runApplication({});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("Usage:"), std::string::npos);
    // Различающий признак ветки: printUsage не добавляет префикс ERROR:,
    // в отличие от printError, которым обрабатываются прочие ошибки
    // разбора аргументов (см. ApplicationRunWithMissingConfigFileReturnsErrorExitCode
    // выше, где ERROR: обязателен).
    EXPECT_EQ(stderrOutput.find("ERROR:"), std::string::npos);
}

// Application::run: конфигурация, соединение и схема в порядке, но
// recoverState() бросает MatchingEngineError — до этой правки catch-ветка
// "Recovery error" (src/application.cpp, вокруг recoverState) не
// исполнялась ни разу ни одним тестом. Активная заявка с NULL-ценой в
// orders — законное состояние по схеме (price INTEGER, без NOT NULL), но
// нарушает доменное правило "у активной заявки обязана быть цена"
// (OrderRepository::loadActive), поэтому OrderRepository кидает
// DatabaseError, а Application::run переводит его в понятное сообщение
// пользователю и код возврата 1, не роняя процесс необработанным
// исключением.
TEST(IntegrationTest, ApplicationRunWithNullPriceOnActiveOrderReturnsRecoveryErrorExitCode){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    cleanupAllTables(conn);

    constexpr int kBrokenOrderId = 902200001;
    // Вставка напрямую по SQL, в обход движка: OrderRepository::save()
    // никогда не производит такую комбинацию (validateOrderChange() не
    // пускает её), поэтому единственный способ получить NULL-цену у
    // активной заявки в этом тесте — вставить строку вручную, эмулируя
    // испорченные данные, оставленные внешним вмешательством.
    conn.execute(
        "INSERT INTO orders (order_id, side, price, initial_quantity, remaining_quantity, "
        "status, sequence_number) VALUES ($1, 'BUY', NULL, 5, 5, 'OPEN', $2)",
        {std::optional<std::string>(std::to_string(kBrokenOrderId)),
            std::optional<std::string>(std::to_string(kBrokenOrderId))});

    testing::internal::CaptureStderr();
    const int exitCode = runApplication({"{\"commands\":[]}"});
    const std::string stderrOutput = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(stderrOutput.find("Failed to recover state from the database"), std::string::npos);

    conn.execute("DELETE FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kBrokenOrderId))});
}

// recoverState() с полностью пустыми orders/processed_commands: обе ветки
// цикла (по активным заявкам и по обработанным командам) выполняются ноль
// раз, а OrderRepository::maxSequenceNumber() получает NULL от MAX() над
// пустой таблицей (её собственная ветка isNull) — до этой правки ни один
// тест не вызывал recoverState() сразу после полной очистки таблиц, всегда
// вставляя данные заранее.
TEST(IntegrationTest, RecoveryWithEmptyDatabaseSucceedsAndBookStaysEmpty){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    cleanupAllTables(conn);

    CommandProcessor processor;
    EXPECT_NO_THROW(recoverState(conn, processor));

    // Восстанавливать нечего — книга остаётся пустой, а не падает и не
    // выдумывает заявки.
    EXPECT_EQ(processor.orderBook().findOrder(1), nullptr);

    // Счётчик последовательности сброшен на 1 (maxSequenceNumber() вернула
    // 0 из-за isNull-ветки), поэтому первая же новая заявка обязана
    // получить sequence_number = 1 и нормально попасть в книгу.
    constexpr int kFreshOrderId = 902300001;
    AddCommand fresh(kFreshOrderId, Side::Buy, 100, 1);
    fresh.commandId_ = "recovery-empty-fresh";
    EXPECT_NO_THROW(processor.process(fresh, conn));
    ASSERT_NE(processor.orderBook().findOrder(kFreshOrderId), nullptr);
    EXPECT_EQ(processor.orderBook().findOrder(kFreshOrderId)->getSequenceNumber(), 1);

    cleanupAllTables(conn);
}

// recoverState(): processed_commands.result может быть NULL (колонка
// nullable) — CommandProcessor::warmCache() тогда прогревает кеш пустым
// ExecutionResult вместо разбора JSON. До этой правки ни один тест не
// доводил NULL до recoverState(): единственная другая вставка NULL-результата
// в этом файле (network_tests.cpp) намеренно обходит recoverState(), чтобы
// проверить противоположный сценарий — конфликт при записи. Здесь же
// значение NULL нужно ИМЕННО прочитать через recoverState() и убедиться,
// что повтор той же команды после восстановления считается уже
// обработанной (кеш прогрет), но без единой сделки — свидетельство, что
// результат восстановлен как пустой, а не разобран из мусора.
TEST(IntegrationTest, RecoveryWarmsCacheWithEmptyResultWhenStoredResultIsNull){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    cleanupAllTables(conn);

    const std::string commandId = "recovery-null-result";
    conn.execute(
        "INSERT INTO processed_commands (command_id, command_type, status, result) "
        "VALUES ($1, 'ADD', 'APPLIED', NULL)",
        {std::optional<std::string>(commandId)});

    CommandProcessor processor;
    EXPECT_NO_THROW(recoverState(conn, processor));

    // Повтор того же command_id обязан быть опознан кешем (Duplicate),
    // но не задеть книгу и не дать ни одной сделки: содержимое команды
    // здесь намеренно не совпадает с тем, что "было" исходно (никакой
    // исходной команды и не было) — единственная причина, по которой
    // process() не пойдёт в движок, это прогретый кеш.
    constexpr int kOrderId = 902400001;
    AddCommand repeat(kOrderId, Side::Buy, 100, 5);
    repeat.commandId_ = commandId;
    bool servedFromCache = false;
    ExecutionResult result = processor.process(repeat, conn, &servedFromCache);

    EXPECT_TRUE(servedFromCache);
    EXPECT_TRUE(result.trades.empty());
    // Книга не тронута: process() вернулся из кеша до вызова движка,
    // поэтому order_id из "повторной" команды в книгу не попал.
    EXPECT_EQ(processor.orderBook().findOrder(kOrderId), nullptr);

    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>(commandId)});
}

// PersistenceService::saveBatch(): её собственная catch-ветка
// (MatchingEngineError -> PersistenceError) не была покрыта ни одним тестом
// — существующие тесты на flushBatch (command_processor_tests.cpp) проверяют
// только счастливый путь. Строка processed_commands с тем же command_id
// вставляется напрямую по SQL заранее, в обход кеша идемпотентности свежего
// CommandProcessor (кеш не прогревается — recoverState() здесь не
// вызывается, как и в network_tests.cpp для того же save()): движок честно
// обрабатывает ADD и кладёт результат в буфер пакета, а сам INSERT при
// flushBatch() упирается в нарушение уникальности command_id и
// перехватывается PersistenceService::saveBatch(), оборачиваясь в
// PersistenceError — то же самое исключение, что и у save(), но с другого
// пути кода.
TEST(IntegrationTest, FlushBatchWithPreexistingCommandIdThrowsPersistenceError){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;
    cleanupAllTables(conn);

    const std::string commandId = "batch-flush-conflict";
    constexpr int kOrderId = 902500001;

    conn.execute(
        "INSERT INTO processed_commands (command_id, command_type, status, result) "
        "VALUES ($1, 'ADD', 'APPLIED', NULL)",
        {std::optional<std::string>(commandId)});

    CommandProcessor processor;
    AddCommand add(kOrderId, Side::Buy, 100, 5);
    add.commandId_ = commandId;
    // Кеш processor'а пуст (recoverState не вызывался) — processBatched
    // честно выполнит сопоставление и положит результат в буфер, ничего
    // ещё не записав в БД.
    EXPECT_NO_THROW(processor.processBatched(add));
    ASSERT_NE(processor.orderBook().findOrder(kOrderId), nullptr);
    EXPECT_EQ(processor.pendingCount(), 1u);

    EXPECT_THROW(processor.flushBatch(conn), PersistenceError);

    // Транзакция saveBatch() откатилась целиком (PgTransaction RAII) —
    // ни строка orders, ни вторая строка processed_commands не должны были
    // просочиться в БД, несмотря на то что движок в памяти уже применил ADD.
    PgResult orderRow = conn.execute("SELECT COUNT(*) FROM orders WHERE order_id = $1",
        {std::optional<std::string>(std::to_string(kOrderId))});
    ASSERT_EQ(orderRow.rowCount(), 1);
    EXPECT_EQ(orderRow.getValue(0, 0), "0");

    conn.execute("DELETE FROM processed_commands WHERE command_id = $1",
        {std::optional<std::string>(commandId)});
}
