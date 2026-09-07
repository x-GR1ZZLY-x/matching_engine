#include "session.hpp"

#include <nlohmann/json.hpp>

#include "exceptions.hpp"
#include "logger.hpp"

namespace matching_engine {

namespace {

// Тот же плоский формат ошибки, что и у RequestRouter (docs/task4/
// 02-network-protocol.md, раздел 3.5) — но MESSAGE_TOO_LARGE рождается на
// уровне заголовка кадра, ещё до того как есть какая-либо полезная
// нагрузка, поэтому строить его через RequestRouter не получится: там
// решения принимаются про уже прочитанное тело, а тут тела ещё нет.
std::string buildMessageTooLarge(const std::string& message) {
    nlohmann::json response;
    response["status"] = "ERROR";
    response["error"] = "MESSAGE_TOO_LARGE";
    response["message"] = message;
    return response.dump();
}

}

Session::Session(boost::asio::ip::tcp::socket socket, const MessageCodec& codec,
    RequestRouter& router, std::function<void()> onFatalShutdown)
    : socket_(std::move(socket)), codec_(codec), router_(router),
      onFatalShutdown_(std::move(onFatalShutdown)) {}

void Session::start() {
    readHeader();
}

void Session::readHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(headerBuffer_),
        [this, self](boost::system::error_code ec, std::size_t /*transferred*/) {
            if (ec) {
                // Разрыв соединения — штатное событие (REQ-NET-10): новых
                // операций не запускаем, сессия разрушится сама, когда
                // последний захваченный shared_ptr выйдет из области
                // видимости.
                if (ec != boost::asio::error::eof &&
                    ec != boost::asio::error::connection_reset &&
                    ec != boost::asio::error::operation_aborted) {
                    Logger::instance().debug(
                        std::string("Session header read failed: ") + ec.message());
                }
                return;
            }

            std::uint32_t payloadSize = 0;
            try {
                payloadSize = codec_.decodeHeader(headerBuffer_);
            } catch (const MessageTooLargeError& e) {
                closeAfterMessageTooLarge(e.what());
                return;
            }
            readBody(payloadSize);
        });
}

void Session::readBody(std::uint32_t payloadSize) {
    auto self = shared_from_this();
    // Размер уже проверен decodeHeader() внутри readHeader() выше — буфер
    // тела выделяется только для значений, не превышающих maxMessageSize
    // (REQ-PROTO-09: проверка до выделения памяти, а не после).
    bodyBuffer_.assign(payloadSize, '\0');
    boost::asio::async_read(socket_, boost::asio::buffer(bodyBuffer_),
        [this, self](boost::system::error_code ec, std::size_t /*transferred*/) {
            if (ec) {
                if (ec != boost::asio::error::eof &&
                    ec != boost::asio::error::connection_reset &&
                    ec != boost::asio::error::operation_aborted) {
                    Logger::instance().debug(
                        std::string("Session body read failed: ") + ec.message());
                }
                return;
            }

            // Ошибка пользователя (битый JSON, неизвестный тип, ...) не
            // закрывает соединение — router_.handle сама строит ответ со
            // статусом ERROR (docs/task4/02-network-protocol.md, раздел 4)
            // для всего, что наследует MatchingEngineError. Но это не
            // единственное, что может случиться внутри: nlohmann::json
            // умеет бросать свои собственные исключения (например, из
            // dump()), а сбой сохранения в БД (PersistenceError) она ловит
            // и превращает в RouteResult::fatal, не бросая наружу. Поэтому
            // вызов router_.handle стоит в этом же try, что и
            // enqueueResponse: непредвиденное исключение из любой из двух
            // операций обязано закрыть только это соединение, а не выйти
            // из обработчика async_read и уронить io_context::run() целиком
            // (REQ-API-07).
            RouteResult routed;
            try {
                routed = router_.handle(bodyBuffer_);
                enqueueResponse(routed.payload);
            } catch (const MessageTooLargeError&) {
                // Легитимный, полностью выполненный ответ не уместился в
                // max_message_size — это не нарушение протокола со стороны
                // клиента (раздел 4 контракта запрещает закрывать
                // соединение молча за корректный запрос на чтение), но и
                // доставить настоящий ответ уже нечем. closeAfterMessageTooLarge
                // строит маленькую диагностику через ту же очередь записи
                // и закрывает соединение после неё — тем же путём, что и
                // при заявленном в заголовке гиганте. Если ответ, который не
                // поместился, был INTERNAL_ERROR после сбоя сохранения в БД
                // (routed.fatal == true), книга в памяти уже разошлась с
                // хранилищем независимо от того, влез ли этот ответ в
                // лимит, — признак фатальности обязан пережить и эту ветку.
                if (routed.fatal) {
                    fatalAfterWrite_ = true;
                }
                closeAfterMessageTooLarge("Response payload exceeds max_message_size");
                return;
            } catch (const std::exception& e) {
                Logger::instance().error(
                    std::string("Session error, closing connection: ") + e.what());
                closeSocket();
                return;
            }

            if (routed.fatal) {
                // Ответ INTERNAL_ERROR уже в очереди записи — соединение
                // закроется тем же механизмом, что и MESSAGE_TOO_LARGE,
                // когда очередь опустеет (writeNext()), и уже оттуда
                // сервис остановится целиком (docs/task4/
                // 02-network-protocol.md, раздел 3.5).
                closeAfterWrite_ = true;
                fatalAfterWrite_ = true;
                return;
            }
            readHeader();
        });
}

void Session::closeAfterMessageTooLarge(const std::string& message) {
    // Ответ идёт через ту же очередь writeQueue_/codec_, что и обычные
    // ответы: сокет не терпит двух одновременных операций записи, а кадр
    // должен жить в члене класса, а не в локальной переменной, которая
    // разрушится раньше, чем завершится async_write. closeAfterWrite_
    // взводится заранее — writeNext() закроет сокет сам, когда очередь
    // опустеет, вместо того чтобы читать следующий (уже недоверенный)
    // кадр (REQ-PROTO-10).
    closeAfterWrite_ = true;
    try {
        enqueueResponse(buildMessageTooLarge(message));
    } catch (const MessageTooLargeError&) {
        // Предел сервера защищает и от кадра, который прислал клиент, и от
        // сообщения, которое строит сам сервер: при достаточно маленьком
        // max_message_size сам этот системный ответ в лимит не помещается.
        // Объяснить клиенту нечего — закрываем соединение без ответа. Здесь
        // же, а не только в writeNext(), нужно продублировать вызов
        // остановки: closeSocket() на этом пути — единственное, что вообще
        // происходит с соединением, никакой async_write не запускается и
        // writeNext() эту ветку никогда не увидит. Если fatalAfterWrite_ уже
        // взведён вызывающей стороной (сбой сохранения в БД), сервис обязан
        // остановиться и тогда, когда даже диагностика не влезла в лимит.
        closeSocket();
        if (fatalAfterWrite_ && onFatalShutdown_) {
            onFatalShutdown_();
        }
    }
}

void Session::enqueueResponse(const std::string& payload) {
    const bool writeInProgress = !writeQueue_.empty();
    writeQueue_.push_back(codec_.encode(payload));
    if (!writeInProgress) {
        writeNext();
    }
}

void Session::writeNext() {
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(writeQueue_.front()),
        [this, self](boost::system::error_code ec, std::size_t /*transferred*/) {
            if (ec) {
                // Клиент отключился, не дочитав ответ (REQ-NET-10), либо
                // запись оборвалась по другой причине — очередь дальше не
                // продвинется сама, поэтому сокет закрывается явно, чтобы
                // сессия не осталась навсегда принимающей, но не отвечающей.
                // fatalAfterWrite_ обязан сработать и на этом пути: если
                // ответ, который не удалось дописать, был INTERNAL_ERROR
                // после сбоя сохранения в БД, книга в памяти уже разошлась
                // с хранилищем независимо от того, прочитал ли клиент
                // ответ, — сервис обязан остановиться так же, как и при
                // успешной записи (docs/task4/02-network-protocol.md,
                // раздел 3.5).
                closeSocket();
                if (fatalAfterWrite_ && onFatalShutdown_) {
                    onFatalShutdown_();
                }
                return;
            }
            writeQueue_.pop_front();
            if (!writeQueue_.empty()) {
                writeNext();
                return;
            }
            if (closeAfterWrite_) {
                closeSocket();
                if (fatalAfterWrite_ && onFatalShutdown_) {
                    onFatalShutdown_();
                }
            }
        });
}

void Session::closeSocket() {
    boost::system::error_code ignored;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    // writeQueue_ здесь намеренно не трогается: closeSocket() достижим и
    // тогда, когда async_write ещё не завершился и держит буфер на
    // writeQueue_.front() (readBody может закрыть сокет из-за ошибки
    // разбора следующего кадра, пока предыдущий ответ ещё пишется, и
    // closeAfterMessageTooLarge — пока пишется большой предыдущий ответ на
    // PRINT). Контракт Asio требует, чтобы буфер, переданный в async_write,
    // жил до вызова её обработчика; writeQueue_.clear() до этого момента —
    // use-after-free. Очередь освобождается сама вместе с сессией, когда
    // не останется ни одной незавершённой операции.
}

}
