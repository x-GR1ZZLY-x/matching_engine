#include<gtest/gtest.h>
#include<chrono>
#include<memory>
#include"order.hpp"
#include"order_book.hpp"
#include"report_printer.hpp"
#include"trade.hpp"

using namespace matching_engine;

// ReportPrinter — единственный класс со std::cout/std::cerr в проекте
// ("Потоки вывода различаются по назначению"), но до сих пор не имел
// собственного тестового файла: покрытие 0% появилось именно из-за этого,
// а не из-за отсутствия вызовов из Application. Тесты здесь перехватывают
// стандартные потоки через testing::internal::Capture*, что не требует ни
// БД, ни сети, ни sleep — только сам класс и его прямые зависимости
// (Trade, OrderBook, Order).

TEST(ReportPrinterTest, PrintTradeWritesFieldsToStdout){
    ReportPrinter printer;
    Trade trade(10, 20, 150, 3);

    testing::internal::CaptureStdout();
    printer.printTrade(trade);
    const std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("TRADE"), std::string::npos);
    EXPECT_NE(output.find("buy=10"), std::string::npos);
    EXPECT_NE(output.find("sell=20"), std::string::npos);
    EXPECT_NE(output.find("price=150"), std::string::npos);
    EXPECT_NE(output.find("quantity=3"), std::string::npos);
}

// Пустая книга — оба цикла (SELL/BUY) выполняют 0 итераций, это отдельная
// ветвь по сравнению с непустой книгой ниже.
TEST(ReportPrinterTest, PrintOrderBookOnEmptyBookPrintsHeadersOnly){
    ReportPrinter printer;
    OrderBook book;

    testing::internal::CaptureStdout();
    printer.printOrderBook(book);
    const std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("ORDER BOOK"), std::string::npos);
    EXPECT_NE(output.find("SELL"), std::string::npos);
    EXPECT_NE(output.find("BUY"), std::string::npos);
}

// Книга с заявками на обеих сторонах — циклы SELL и BUY проходят хотя бы
// одну итерацию каждый, покрывая противоположную ветвь по сравнению с
// пустой книгой выше.
TEST(ReportPrinterTest, PrintOrderBookWithOrdersPrintsPriceAndQuantity){
    ReportPrinter printer;
    OrderBook book;
    book.addOrder(std::make_shared<Order>(1, Side::Buy, 100, 5, 5, 1, OrderStatus::Open));
    book.addOrder(std::make_shared<Order>(2, Side::Sell, 110, 7, 7, 2, OrderStatus::Open));

    testing::internal::CaptureStdout();
    printer.printOrderBook(book);
    const std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("100 5"), std::string::npos);
    EXPECT_NE(output.find("110 7"), std::string::npos);
}

TEST(ReportPrinterTest, PrintErrorWritesToStderrWithPrefix){
    ReportPrinter printer;

    testing::internal::CaptureStderr();
    printer.printError("something went wrong");
    const std::string output = testing::internal::GetCapturedStderr();

    EXPECT_EQ(output, "ERROR: something went wrong\n");
}

TEST(ReportPrinterTest, PrintUsageWritesToStderrVerbatim){
    ReportPrinter printer;

    testing::internal::CaptureStderr();
    printer.printUsage("Usage: matching_engine ...\n");
    const std::string output = testing::internal::GetCapturedStderr();

    EXPECT_EQ(output, "Usage: matching_engine ...\n");
}

TEST(ReportPrinterTest, PrintResponseWritesToStdoutWithNewline){
    ReportPrinter printer;

    testing::internal::CaptureStdout();
    printer.printResponse("{\"status\":\"OK\"}");
    const std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(output, "{\"status\":\"OK\"}\n");
}

TEST(ReportPrinterTest, PrintReplaySummaryIncludesAllCounters){
    ReportPrinter printer;

    testing::internal::CaptureStdout();
    printer.printReplaySummary(100, 42, 5, 3, std::chrono::milliseconds(1234));
    const std::string output = testing::internal::GetCapturedStdout();

    EXPECT_NE(output.find("REPLAY SUMMARY"), std::string::npos);
    EXPECT_NE(output.find("processed=100"), std::string::npos);
    EXPECT_NE(output.find("trades=42"), std::string::npos);
    EXPECT_NE(output.find("duplicates=5"), std::string::npos);
    EXPECT_NE(output.find("skipped=3"), std::string::npos);
    EXPECT_NE(output.find("elapsed_ms=1234"), std::string::npos);
}
