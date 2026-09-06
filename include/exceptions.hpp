#pragma once

#include <stdexcept>
#include <string>

namespace matching_engine {

class MatchingEngineError : public std::runtime_error {
public:
    explicit MatchingEngineError(const std::string& message)
        : std::runtime_error(message) {}
};

class OrderError : public MatchingEngineError{
public:
    explicit OrderError(const std::string& message)
        : MatchingEngineError(message) {}
};

class ParseError : public MatchingEngineError{
public:
    explicit ParseError(const std::string& message)
        : MatchingEngineError(message) {}
};

class DuplicateOrderError : public MatchingEngineError{
public:
    explicit DuplicateOrderError(const std::string& message)
        : MatchingEngineError(message) {}
};

class OrderBookError : public MatchingEngineError{
public:
    explicit OrderBookError(const std::string& message)
        : MatchingEngineError(message) {}
};

class DatabaseError : public MatchingEngineError{
public:
    explicit DatabaseError(const std::string& message)
        : MatchingEngineError(message) {}
};

class ConfigError : public MatchingEngineError{
public:
    explicit ConfigError(const std::string& message)
        : MatchingEngineError(message) {}
};

// Кадр объявляет длину полезной нагрузки больше допустимого максимума
// (server.max_message_size). Бросается MessageCodec::decodeHeader сразу
// после разбора заголовка, до выделения памяти под тело кадра. Это
// нарушение протокола, а не ошибка пользователя: читать поток дальше,
// доверяя объявленной длине источника, который контракт уже нарушил,
// нельзя, поэтому соединение, получившее такой кадр, закрывается.
// Наследует MatchingEngineError по конвенции проекта, поэтому обработчик
// сессии обязан перехватывать его раньше generic catch(const
// MatchingEngineError&) — тем же способом, каким уже выделен ниже
// PersistenceError, — и закрывать соединение, а не отвечать ERROR и
// продолжать чтение, как для обычной ошибки пользователя (REQ-PROTO-10,
// docs/task4/02-network-protocol.md, раздел 4).
class MessageTooLargeError : public MatchingEngineError{
public:
    explicit MessageTooLargeError(const std::string& message)
        : MatchingEngineError(message) {}
};

// Сбой сохранения результата команды в БД (задача 07, REQ-TX-03). Наследует
// MatchingEngineError по конвенции проекта, поэтому Application::processCommand
// обязан перехватывать его раньше generic catch(const MatchingEngineError&) и
// пробрасывать наверх — иначе тот перехват проглотит его как обычную
// командную ошибку. К моменту, когда это исключение долетает наружу, книга
// заявок в памяти уже изменена, а запись в БД не удалась: продолжать работу
// нельзя, только аварийно (но штатно, без std::terminate) завершить процесс.
class PersistenceError : public MatchingEngineError{
public:
    explicit PersistenceError(const std::string& message)
        : MatchingEngineError(message) {}
};

}