#pragma once
#include<chrono>
#include<vector>
#include"trade.hpp"
#include"order_book.hpp"

namespace matching_engine{

class ReportPrinter{
public:
    void printTrade(const Trade& trade) const;
    void printOrderBook(const OrderBook& book) const;
    void printError(const std::string& message) const;

    // Справка по использованию (нет аргументов вовсе) — не ошибка, поэтому
    // без префикса "ERROR:", но в stderr, как и printError: это тоже
    // пользовательский вывод, а не результат работы движка, и не должен
    // попадать в stdout вперемешку с TRADE/ORDER BOOK/REPLAY SUMMARY.
    void printUsage(const std::string& usage) const;

    // Итоговая сводка режима --replay. processedCommands и trades — счётчики только для
    // команд со статусом Applied: processedCommands равен числу строк,
    // добавленных в этом прогоне в processed_commands, trades — числу
    // строк, добавленных в trades. Команда, обслуженная из кеша
    // идемпотентности (повторный command_id), в БД ничего не пишет и
    // попадает в duplicates, а не в processedCommands/trades — иначе её
    // сделки считались бы дважды. skippedLines — строки, пропущенные из-за
    // ошибок разбора или доменных ошибок. Единственное место, печатающее
    // сводку — ReportPrinter остаётся единственным классом со std::cout.
    void printReplaySummary(long long processedCommands, long long trades,
        long long duplicates, long long skippedLines,
        std::chrono::milliseconds elapsed) const;

    // Ответ сервера matching-engine-client: полезная нагрузка
    // кадра ответа уже готовая строка JSON (её строит сервер, а клиент
    // только доставляет байты через Client — REQ-CLI-04), поэтому здесь
    // печатается буквально, без разбора и повторной сериализации.
    void printResponse(const std::string& response) const;
};

}