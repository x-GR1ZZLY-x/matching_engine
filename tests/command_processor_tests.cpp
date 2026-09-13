#include<gtest/gtest.h>
#include<memory>
#include<optional>
#include<string>
#include<vector>
#include"command.hpp"
#include"command_processor.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"
#include"pg_result.hpp"
#include"test_database.hpp"

using namespace matching_engine;
using matching_engine::test::g_lastConnectFailure;
using matching_engine::test::tryConnect;

namespace{

OrderChange makeChange(int id, Side side, std::optional<int> price, int initialQuantity,
    int remainingQuantity, OrderStatus status, long long sequenceNumber){
    return OrderChange{id, side, price, initialQuantity, remainingQuantity,
        status, sequenceNumber};
}

}

// Сериализация в processed_commands.result и
// разбор обратно живут рядом в CommandProcessor и обязаны быть точной парой —
// это тот самый round-trip тест, который ловит расхождение форматов, не
// требуя базы.
TEST(CommandProcessorTest, SerializeParseRoundTripPreservesTradesAndOrderChanges){
    ExecutionResult result;
    result.trades.push_back(Trade(1, 2, 100, 5));
    result.trades.push_back(Trade(3, 4, 150, 10));
    result.orderChanges.push_back(
        makeChange(1, Side::Buy, 100, 10, 5, OrderStatus::PartiallyFilled, 7));
    result.orderChanges.push_back(
        makeChange(2, Side::Sell, std::nullopt, 20, 0, OrderStatus::Cancelled, 8));

    ExecutionResult parsed =
        CommandProcessor::parseResult(CommandProcessor::serializeResult(result));

    ASSERT_EQ(parsed.trades.size(), 2u);
    EXPECT_EQ(parsed.trades[0].getBuyOrderId(), 1);
    EXPECT_EQ(parsed.trades[0].getSellOrderId(), 2);
    EXPECT_EQ(parsed.trades[0].getPrice(), 100);
    EXPECT_EQ(parsed.trades[0].getQuantity(), 5);
    EXPECT_EQ(parsed.trades[1].getBuyOrderId(), 3);
    EXPECT_EQ(parsed.trades[1].getSellOrderId(), 4);
    EXPECT_EQ(parsed.trades[1].getPrice(), 150);
    EXPECT_EQ(parsed.trades[1].getQuantity(), 10);

    ASSERT_EQ(parsed.orderChanges.size(), 2u);
    EXPECT_EQ(parsed.orderChanges[0].id, 1);
    EXPECT_EQ(parsed.orderChanges[0].side, Side::Buy);
    ASSERT_TRUE(parsed.orderChanges[0].price.has_value());
    EXPECT_EQ(*parsed.orderChanges[0].price, 100);
    EXPECT_EQ(parsed.orderChanges[0].initialQuantity, 10);
    EXPECT_EQ(parsed.orderChanges[0].remainingQuantity, 5);
    EXPECT_EQ(parsed.orderChanges[0].status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(parsed.orderChanges[0].sequenceNumber, 7);

    EXPECT_EQ(parsed.orderChanges[1].id, 2);
    EXPECT_EQ(parsed.orderChanges[1].side, Side::Sell);
    // MARKET-заявка сериализуется с price == nullopt (SQL NULL) — критично
    // отличать это от отсутствия поля или числа 0.
    EXPECT_FALSE(parsed.orderChanges[1].price.has_value());
    EXPECT_EQ(parsed.orderChanges[1].status, OrderStatus::Cancelled);
}

TEST(CommandProcessorTest, SerializeParseRoundTripHandlesEmptyResult){
    ExecutionResult result;

    ExecutionResult parsed =
        CommandProcessor::parseResult(CommandProcessor::serializeResult(result));

    EXPECT_TRUE(parsed.trades.empty());
    EXPECT_TRUE(parsed.orderChanges.empty());
}

// MODIFY кладёт в orderChanges два снимка одной заявки — старый в начале,
// новый в конце: порядок значим, побеждает
// последняя запись с этим id. Разбор обязан сохранить порядок массива.
TEST(CommandProcessorTest, SerializeParseRoundTripPreservesOrderOfDuplicateIds){
    ExecutionResult result;
    result.orderChanges.push_back(makeChange(5, Side::Buy, 100, 10, 10, OrderStatus::Open, 1));
    result.orderChanges.push_back(makeChange(5, Side::Buy, 110, 8, 8, OrderStatus::Open, 2));

    ExecutionResult parsed =
        CommandProcessor::parseResult(CommandProcessor::serializeResult(result));

    ASSERT_EQ(parsed.orderChanges.size(), 2u);
    EXPECT_EQ(parsed.orderChanges[0].sequenceNumber, 1);
    EXPECT_EQ(parsed.orderChanges[1].sequenceNumber, 2);
}

// restoreOrder кладёт заявки в книгу через
// MatchingEngine::restore -> OrderBook::restore, минуя сопоставление — даже
// пересекающиеся по цене BUY/SELL не должны породить сделку.
TEST(CommandProcessorTest, RestoreOrderPutsCrossingOrdersIntoBookWithoutMatching){
    CommandProcessor processor;

    auto buy = std::make_shared<Order>(1, Side::Buy, 100, 10, 10, 1, OrderStatus::Open);
    auto sell = std::make_shared<Order>(2, Side::Sell, 90, 5, 5, 2, OrderStatus::Open);

    processor.restoreOrder(buy);
    processor.restoreOrder(sell);

    // findOrder(...) != nullptr одной наличия в книге недостаточно: частично
    // исполненная заявка тоже осталась бы в книге. Проверяем, что остатки не
    // уменьшились — иначе сопоставление всё же произошло.
    ASSERT_NE(processor.orderBook().findOrder(1), nullptr);
    ASSERT_NE(processor.orderBook().findOrder(2), nullptr);
    EXPECT_EQ(processor.orderBook().findOrder(1)->getQuantity(), 10);
    EXPECT_EQ(processor.orderBook().findOrder(2)->getQuantity(), 5);
}

// Колонка processed_commands.result
// nullable — прогрев кеша обязан пережить NULL, а не упасть при старте.
TEST(CommandProcessorTest, WarmCacheAcceptsNullResultWithoutThrowing){
    CommandProcessor processor;

    EXPECT_NO_THROW(processor.warmCache("cmd-1", std::nullopt));
}

// parseResult() бросает DatabaseError напрямую на синтаксически невалидном
// JSON (nlohmann::json::parse_error перехвачен внутри и обёрнут) — этот путь
// до сих пор проверялся только опосредованно через warmCache.
TEST(CommandProcessorTest, ParseResultThrowsDatabaseErrorOnInvalidJson){
    EXPECT_THROW(CommandProcessor::parseResult("{not valid json"), DatabaseError);
}

// Синтаксически валидный JSON, но без обязательного ключа "trades" —
// нашёл другую ветку catch(nlohmann::json::exception) в parseResult():
// j.at("trades") бросает out_of_range (наследник json::exception), а не
// parse_error, как в тесте выше.
TEST(CommandProcessorTest, ParseResultThrowsDatabaseErrorOnMissingRequiredKey){
    EXPECT_THROW(CommandProcessor::parseResult("{\"order_changes\":[]}"), DatabaseError);
}

// commandId_ присутствует, но пустая строка — вторая часть условия
// "!command.commandId_ || command.commandId_->empty()", отдельная от
// commandId_ == nullopt (ProcessBatchedWithoutCommandIdThrowsParseError
// ниже покрывает только nullopt).
TEST(CommandProcessorTest, ProcessBatchedWithEmptyCommandIdStringThrowsParseError){
    CommandProcessor processor;
    AddCommand command(1, Side::Buy, 100, 5);
    command.commandId_ = std::string("");

    EXPECT_THROW(processor.processBatched(command), ParseError);
}

// warmCache() с валидным (не NULL) result — путь, отличный от
// WarmCacheAcceptsNullResultWithoutThrowing выше: разбирает JSON и кладёт
// результат в кеш. Проверяется без обращения к БД: processBatched() читает
// только cache_ и engine_ в памяти, поэтому повтор того же command_id после
// прогрева обязан вернуть ИМЕННО прогретый результат (сделку с ценой 999,
// которой ни разу не было в реальном сопоставлении), а не пересчитать его
// заново через MatchingEngine.
TEST(CommandProcessorTest, WarmCacheWithValidResultServesItOnRepeatWithoutMatching){
    CommandProcessor processor;
    ExecutionResult warmed;
    warmed.trades.push_back(Trade(1, 2, 999, 1));
    processor.warmCache("warm-cmd", CommandProcessor::serializeResult(warmed));

    AddCommand repeat(500, Side::Buy, 100, 1);
    repeat.commandId_ = "warm-cmd";
    bool servedFromCache = false;
    ExecutionResult result = processor.processBatched(repeat, &servedFromCache);

    EXPECT_TRUE(servedFromCache);
    ASSERT_EQ(result.trades.size(), 1u);
    EXPECT_EQ(result.trades[0].getPrice(), 999);
    // Заявка из "повторной" команды не должна была попасть в книгу —
    // обработка не дошла до движка вовсе.
    EXPECT_EQ(processor.orderBook().findOrder(500), nullptr);
}

// warmCache() с повреждённым (не NULL, но невалидным) result оборачивает
// исключение parseResult в новый DatabaseError с указанием command_id —
// путь, отдельный от WarmCacheAcceptsNullResultWithoutThrowing.
TEST(CommandProcessorTest, WarmCacheWithCorruptedResultThrowsDatabaseError){
    CommandProcessor processor;

    EXPECT_THROW(processor.warmCache("bad-cmd", std::optional<std::string>("{not valid json")),
        DatabaseError);
}

// processBatched() без command_id — тот же fail-fast, что и у process():
// проверка выполняется до всякого обращения к кешу или движку.
TEST(CommandProcessorTest, ProcessBatchedWithoutCommandIdThrowsParseError){
    CommandProcessor processor;
    AddCommand command(1, Side::Buy, 100, 5);
    // commandId_ остаётся nullopt.

    EXPECT_THROW(processor.processBatched(command), ParseError);
}

// Повтор command_id внутри одного пакета обязан быть опознан кешем сразу
// (до flushBatch) — иначе повторный вызов processBatched() выполнил бы
// сопоставление ещё раз (см. докстроку CommandProcessor::processBatched в
// заголовке). Проверяется без БД: pendingCount() должен вырасти только на
// первый вызов.
TEST(CommandProcessorTest, ProcessBatchedServesRepeatFromCacheWithinSameBatch){
    CommandProcessor processor;
    AddCommand first(600, Side::Buy, 100, 5);
    first.commandId_ = "batch-repeat";

    bool servedFirst = true;
    ExecutionResult resultFirst = processor.processBatched(first, &servedFirst);
    EXPECT_FALSE(servedFirst);
    EXPECT_EQ(processor.pendingCount(), 1u);

    AddCommand repeat(601, Side::Buy, 100, 5);
    repeat.commandId_ = "batch-repeat";
    bool servedSecond = false;
    ExecutionResult resultSecond = processor.processBatched(repeat, &servedSecond);

    EXPECT_TRUE(servedSecond);
    // Буфер не вырос — повтор не добавил вторую запись в пакет.
    EXPECT_EQ(processor.pendingCount(), 1u);
    EXPECT_EQ(resultFirst.trades.size(), resultSecond.trades.size());
}

// flushBatch() с пустым буфером — no-op по докстроке в заголовке: ни одна
// транзакция не открывается, вызов не должен бросать даже без БД (реальное
// соединение здесь не открывается вовсе — ветка возвращается раньше, чем
// connection используется хоть как-то).
TEST(CommandProcessorTest, FlushBatchWithEmptyBufferDoesNotTouchConnection){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    CommandProcessor processor;
    EXPECT_NO_THROW(processor.flushBatch(conn));
}

// Интеграционный сценарий: несколько команд через processBatched (без
// обращения к БД на этом шаге), затем один flushBatch() — покрывает
// PersistenceService::saveBatch и, транзитивно, saveBatch()/insertBatch() всех
// трёх репозиториев и executeBatchedInsert (sql_batch_insert.hpp), ни разу
// не выполнявшиеся до этой правки.
TEST(CommandProcessorTest, ProcessBatchedThenFlushPersistsOrdersAndTrades){
    auto connOpt = tryConnect();
    if(!connOpt){
        GTEST_SKIP() << "База данных недоступна: " << g_lastConnectFailure;
    }
    auto& conn = *connOpt;

    constexpr int kSellId = 901800001;
    constexpr int kBuyId = 901800002;
    conn.execute("DELETE FROM trades WHERE buy_order_id = $1 OR sell_order_id = $1",
        {std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM orders WHERE order_id = $1 OR order_id = $2",
        {std::optional<std::string>(std::to_string(kSellId)),
            std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("batch-flush-sell"),
            std::optional<std::string>("batch-flush-buy")});

    CommandProcessor processor;

    AddCommand sell(kSellId, Side::Sell, 100, 10);
    sell.commandId_ = "batch-flush-sell";
    processor.processBatched(sell);

    AddCommand buy(kBuyId, Side::Buy, 100, 4);
    buy.commandId_ = "batch-flush-buy";
    ExecutionResult buyResult = processor.processBatched(buy);
    ASSERT_EQ(buyResult.trades.size(), 1u);

    EXPECT_EQ(processor.pendingCount(), 2u);

    // До flushBatch ничего не должно быть записано в БД.
    PgResult before = conn.execute(
        "SELECT COUNT(*) FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("batch-flush-sell"),
            std::optional<std::string>("batch-flush-buy")});
    ASSERT_EQ(before.rowCount(), 1);
    EXPECT_EQ(before.getValue(0, 0), "0");

    processor.flushBatch(conn);

    EXPECT_EQ(processor.pendingCount(), 0u);

    PgResult after = conn.execute(
        "SELECT COUNT(*) FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("batch-flush-sell"),
            std::optional<std::string>("batch-flush-buy")});
    ASSERT_EQ(after.rowCount(), 1);
    EXPECT_EQ(after.getValue(0, 0), "2");

    PgResult trades = conn.execute(
        "SELECT COUNT(*) FROM trades WHERE buy_order_id = $1 AND sell_order_id = $2",
        {std::optional<std::string>(std::to_string(kBuyId)),
            std::optional<std::string>(std::to_string(kSellId))});
    ASSERT_EQ(trades.rowCount(), 1);
    EXPECT_EQ(trades.getValue(0, 0), "1");

    conn.execute("DELETE FROM trades WHERE buy_order_id = $1 OR sell_order_id = $1",
        {std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM orders WHERE order_id = $1 OR order_id = $2",
        {std::optional<std::string>(std::to_string(kSellId)),
            std::optional<std::string>(std::to_string(kBuyId))});
    conn.execute("DELETE FROM processed_commands WHERE command_id = $1 OR command_id = $2",
        {std::optional<std::string>("batch-flush-sell"),
            std::optional<std::string>("batch-flush-buy")});
}
