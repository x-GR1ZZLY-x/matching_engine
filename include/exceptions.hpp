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

// Значение не проходит проверку предметной области (задача 05, код
// INVALID_ORDER, docs/task4/02-network-protocol.md, раздел 3.5):
// неположительная цена или количество, неизвестная сторона. Наследует
// ParseError (а не MatchingEngineError напрямую), поэтому все существующие
// catch(const ParseError&) и тесты, стоящие на этом типе, продолжают
// работать без изменений — RequestRouter ловит этот более специфичный тип
// раньше базового ParseError, чтобы вернуть код INVALID_ORDER, а не
// INVALID_REQUEST.
class InvalidOrderValueError : public ParseError{
public:
    explicit InvalidOrderValueError(const std::string& message)
        : ParseError(message) {}
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

// Ошибка клиентской стороны сетевого протокола (задача 04, REQ-CLI-04):
// не удалось подключиться, операция чтения/записи завершилась ошибкой или
// разрывом соединения, либо истёк таймаут ожидания. Наследует
// MatchingEngineError по конвенции проекта. Бросается только классом
// Client — сервер о ней не знает, у него симметричный, но отдельный путь
// ошибок (MessageTooLargeError и обычные ответы со статусом ERROR).
class NetworkError : public MatchingEngineError{
public:
    explicit NetworkError(const std::string& message)
        : MatchingEngineError(message) {}
};

// Частный случай NetworkError: операция не завершилась не потому, что
// соединение оборвалось, а потому что истёк клиентский таймаут ожидания
// (задача 04). Отличать эти два случая по типу нужно там, где обрыв
// соединения и молчание сервера должны проверяться по-разному — например,
// в тесте на MESSAGE_TOO_LARGE, где важно убедиться именно в закрытии
// соединения, а не в том, что сервер просто не ответил вовремя.
class NetworkTimeoutError : public NetworkError{
public:
    explicit NetworkTimeoutError(const std::string& message)
        : NetworkError(message) {}
};

}