#pragma once

#include<memory>
#include<optional>
#include<string>
#include"command.hpp"
#include"command_cache.hpp"
#include"execution_result.hpp"
#include"matching_engine.hpp"
#include"order_book.hpp"
#include"persistence_service.hpp"
#include"pg_connection.hpp"

namespace matching_engine{

// Ядро ДЗ-3: связывает сопоставление (MatchingEngine), кеш идемпотентности
// (CommandCache) и сохранение в БД (PersistenceService) в единый поток
// обработки одной изменяющей команды (docs/plan.md, "Идемпотентность и
// транзакция"). Об argv и std::cout не знает — это остаётся обязанностью
// Application и ReportPrinter; JSON здесь всё же используется — для
// сериализации ExecutionResult в processed_commands.result.
class CommandProcessor{
public:
    // command — изменяющая состояние команда (ADD/CANCEL/MODIFY). PRINT
    // сюда не передаётся: он не требует ни command_id, ни кеша, ни
    // транзакции, а нуждается лишь в текущем состоянии книги — Application
    // читает его напрямую через orderBook() и печатает сама.
    //
    // connection используется только на время этого вызова и не хранится
    // полем — как и у репозиториев (см. include/order_repository.hpp),
    // это исключает сценарий обращения к соединению после его перемещения.
    //
    // Порядок: command_id отсутствует -> ParseError; command_id уже в
    // кеше -> вернуть прошлый результат, ничего не выполняя и не читая БД;
    // иначе -> сопоставление в памяти, затем одна транзакция БД, затем
    // запись результата в кеш (REQ-IDEM-01..06, REQ-TX-01/02).
    //
    // servedFromCache, если не nullptr, выставляется в true на ветке
    // раннего возврата из кеша (команда не записывалась в БД в этом
    // вызове) и в false, когда результат получен свежей записью. Нужен
    // вызывающей стороне (Application::runReplay, задача 10), чтобы
    // отличать реально сохранённые команды от повторов идемпотентности —
    // по числу сделок это неразличимо. Параметр по умолчанию сохраняет
    // существующие вызовы без изменений.
    ExecutionResult process(const Command& command, PgConnection& connection,
        bool* servedFromCache = nullptr);

    const OrderBook& orderBook() const noexcept;

    // Восстановление при старте (задача 08, RecoveryService, см.
    // src/recovery_service.cpp): узкие методы вместо доступа к engine_/cache_
    // целиком. restoreOrder кладёт уже готовую заявку в книгу мимо
    // сопоставления (MatchingEngine::restore -> OrderBook::restore, задача
    // 05) — сделок при этом не возникает, даже если в книге уже есть
    // встречная заявка с пересекающейся ценой. warmCache заполняет кеш
    // идемпотентности одной записью processed_commands; resultJson == nullopt
    // (NULL-колонка) даёт пустой ExecutionResult, а не ошибку.
    void restoreOrder(std::shared_ptr<Order> order);
    void warmCache(const std::string& commandId, const std::optional<std::string>& resultJson);

    // Сериализация ExecutionResult <-> processed_commands.result (JSONB) —
    // симметричная пара в одном месте: process() сериализует перед
    // сохранением, warmCache() разбирает обратно при прогреве. Публичны и
    // статичны ради round-trip теста (docs/tasks/task-08.md, "Условия
    // работы") — иного смысла, кроме кодека формата, не несут и полей
    // класса не читают.
    static std::string serializeResult(const ExecutionResult& result);
    static ExecutionResult parseResult(const std::string& json);

private:
    CommandCache cache_;
    MatchingEngine engine_;
    PersistenceService persistence_;
};

}
