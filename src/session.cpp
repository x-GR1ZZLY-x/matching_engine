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
    RequestRouter& router)
    : socket_(std::move(socket)), codec_(codec), router_(router) {}

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
            // статусом ERROR (docs/task4/02-network-protocol.md, раздел
            // 4) и не бросает исключений. try/catch здесь — это граница
            // одной команды (тот же инвентарь, что и у Application::
            // processCommand): единственный способ попасть сюда — это
            // по-настоящему исключительная ситуация (например, сам ответ
            // не уместился в max_message_size), а не ошибка пользователя.
            // Такое соединение закрывается, не роняя ни сервер, ни другие
            // сессии (REQ-NET-01, REQ-NET-10).
            try {
                enqueueResponse(router_.handle(bodyBuffer_));
            } catch (const std::exception& e) {
                Logger::instance().error(
                    std::string("Session error, closing connection: ") + e.what());
                closeSocket();
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
        // Объяснить клиенту нечего — закрываем соединение без ответа.
        closeSocket();
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
                closeSocket();
                return;
            }
            writeQueue_.pop_front();
            if (!writeQueue_.empty()) {
                writeNext();
                return;
            }
            if (closeAfterWrite_) {
                closeSocket();
            }
        });
}

void Session::closeSocket() {
    boost::system::error_code ignored;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    // На пути ошибки записи (writeNext()) элемент, который не удалось
    // отправить, иначе остался бы в очереди: сокет уже закрыт, поэтому это
    // ненаблюдаемо снаружи, но writeQueue_.empty() как признак "идёт
    // запись" обязан оставаться правдой и после закрытия по ошибке.
    writeQueue_.clear();
}

}
