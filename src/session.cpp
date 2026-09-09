#include "session.hpp"

#include <optional>

#include "exceptions.hpp"
#include "logger.hpp"
#include "response_serializer.hpp"

namespace matching_engine {

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
                // Тела ещё нет (заголовок сам объявил недопустимый размер) —
                // command_id эхировать нечего, в отличие от RESPONSE_TOO_LARGE
                // ниже, где запрос уже прочитан целиком.
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
                // max_message_size — раздел 4.1 контракта называет это
                // ошибкой пользователя, а не протокола: кадр запроса
                // прочитан целиком, позиция в потоке известна, короткий
                // ответ об ошибке доставим. Поэтому соединение остаётся
                // живым, и после постановки RESPONSE_TOO_LARGE в очередь
                // записи читается следующий кадр — так же, как после любой
                // другой ошибки пользователя чуть ниже по этой функции.
                //
                // Исключение — routed.fatal: ответ, который не поместился,
                // был INTERNAL_ERROR после сбоя сохранения в БД. Книга в
                // памяти уже разошлась с хранилищем независимо от того,
                // влез ли этот конкретный ответ в лимит, поэтому признак
                // фатальности обязан пережить и эту ветку — соединение
                // закрывается после отправки RESPONSE_TOO_LARGE, и сервис
                // останавливается, как и на прямом пути ниже.
                if (routed.fatal) {
                    fatalAfterWrite_ = true;
                    closeAfterWrite_ = true;
                }

                // routed.orderId заполнен только на успешно выполненной
                // изменяющей команде (RequestRouter::handleDomainCommand):
                // заявка уже сопоставлена, сделки записаны, транзакция
                // закоммичена — потерялся только этот конкретный ответ
                // (docs/task4/02-network-protocol.md, раздел 4.1,
                // "Изменяющая команда: команда выполнена, ответ не
                // доставлен"). message обязан явно сообщать об этом, а не
                // выглядеть как отказ: клиент, увидевший обычный ERROR,
                // разумно решил бы, что заявка не принята, и отправил бы её
                // заново с новым command_id — идемпотентность от дубля не
                // защищает.
                std::string message = "Response payload exceeds max_message_size";
                if (routed.orderId.has_value()) {
                    message = "Command executed and saved successfully, "
                              "but the response payload exceeds max_message_size";
                    Logger::instance().error(
                        "RESPONSE_TOO_LARGE for a completed command: command_id=" +
                        (routed.commandId ? *routed.commandId : std::string("<none>")) +
                        " order_id=" + std::to_string(*routed.orderId) +
                        " trades=" + std::to_string(routed.tradesCount));
                }

                try {
                    // routed.commandId — то же эхо, что уже было в
                    // payload'е, который не поместился (раздел 3.5): запрос
                    // прочитан целиком, эхировать есть что, в отличие от
                    // MESSAGE_TOO_LARGE выше, где тела ещё не было.
                    enqueueResponse(
                        ResponseSerializer::error(routed.commandId, "RESPONSE_TOO_LARGE", message));
                } catch (const MessageTooLargeError&) {
                    // Предел настолько мал, что даже это короткое сообщение
                    // не помещается. loadConfig отвергает такую
                    // конфигурацию нижней границей server.max_message_size
                    // (раздел 4.1), поэтому в штатной работе сюда не
                    // попасть; тесты, строящие Session с ServerConfig
                    // напрямую, минуя загрузку конфигурации, могут. Тогда
                    // объяснить клиенту нечего — закрываем соединение без
                    // ответа, как и на пути заявленного в заголовке
                    // гиганта.
                    closeSocket();
                    notifyFatalShutdown();
                    return;
                }
                // closeAfterWrite_ уже означает "читать больше нечего" —
                // взведён либо только что (routed.fatal), либо снаружи,
                // beginClose() при остановке сервера (docs/task4/
                // 01-service-lifecycle.md, раздел 4.1, шаг 6): новый кадр в
                // этом случае не читаем, иначе конвейер команд от клиента не
                // даёт очереди записи опустеть (REQ-THR-11, REQ-EXT-08).
                if (!routed.fatal && !closeAfterWrite_) {
                    readHeader();
                }
                return;
            } catch (const std::exception& e) {
                Logger::instance().error(
                    std::string("Session error, closing connection: ") + e.what());
                closeSocket();
                // routed.fatal уже мог быть true (PersistenceError успешно
                // обработан router_.handle, а второе исключение — из
                // enqueueResponse — прилетело уже после этого): признак
                // фатальности не должен потеряться на этой ветке так же, как
                // и на трёх остальных точках закрытия сессии.
                if (routed.fatal) {
                    fatalAfterWrite_ = true;
                }
                notifyFatalShutdown();
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
            // closeAfterWrite_ уже означает "читать больше нечего": чтение
            // тела, которое сейчас обработано, было запущено раньше и могло
            // застать очередь пустой, но снаружи — beginClose() при
            // остановке сервера (docs/task4/01-service-lifecycle.md, раздел
            // 4.1, шаг 6) — успел выполниться более ранний обработчик в
            // том же io_context и взвести признак. Новый кадр в этом случае
            // не читаем, иначе клиент, шлющий команды конвейером, не даёт
            // очереди записи опустеть, и остановка становится принудительной
            // (REQ-THR-11, REQ-EXT-08).
            if (!closeAfterWrite_) {
                readHeader();
            }
        });
}

void Session::beginClose() {
    // closeAfterWrite_ взводится безусловно, а не только на ветке "очередь
    // не пуста": closeSocket() ниже отменяет лишь ожидающее чтение, но не
    // трогает обработчик уже успешно завершившегося чтения, который может
    // остаться в очереди io_context и отработать следом — без признака он
    // дочитал бы следующий кадр (readHeader()/readBody() не проверяют его
    // сами по себе, а закрытый сокет к тому моменту уже не спасёт от
    // повторного захода). Признак в обеих ветках означает одно и то же:
    // "идёт закрытие, читать больше нечего" — та же проверка
    // (!closeAfterWrite_) на выходе из readBody(), что уже останавливает
    // конвейер команд, останавливает и этот случай.
    //
    // Идемпотентен по построению: повторный вызов на сессии, уже
    // закрывающейся (closeAfterWrite_ уже true) или уже закрытой (очередь
    // пуста, сокет уже закрыт closeSocket()'ом), не делает ничего нового —
    // closeSocket() на уже закрытом сокете и повторное взведение уже
    // взведённого признака безвредны. Это существенно: повторный SIGTERM
    // во время уже идущей остановки обходит реестр сессий снова (раздел
    // 2.4 документа).
    closeAfterWrite_ = true;
    if (writeQueue_.empty()) {
        closeSocket();
    }
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
        // command_id эхировать нечего: тела ещё нет, заголовок кадра сам
        // объявил недопустимый размер (см. комментарий в readHeader()).
        enqueueResponse(ResponseSerializer::error(std::nullopt, "MESSAGE_TOO_LARGE", message));
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
        notifyFatalShutdown();
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
                notifyFatalShutdown();
                return;
            }
            writeQueue_.pop_front();
            if (!writeQueue_.empty()) {
                writeNext();
                return;
            }
            if (closeAfterWrite_) {
                closeSocket();
                notifyFatalShutdown();
            }
        });
}

void Session::notifyFatalShutdown() {
    // Несколько мест кода готовы сообщить о сбое сохранения (закрытие после
    // успешной записи, закрытие после ошибки записи, и обе ветки "даже
    // диагностика не влезла" — closeAfterMessageTooLarge() и readBody()), а
    // при конвейерной обработке пары из них могут сработать на одном и том
    // же соединении: одна синхронно, closeSocket()'ом эту же сессию, другая —
    // позже, когда уже запущенная асинхронная запись более раннего ответа
    // завершится ошибкой из-за того же закрытого сокета. fatalNotified_
    // делает вызов onFatalShutdown_ идемпотентным, не полагаясь на то, что
    // ровно одна из этих точек сработает на любом сценарии.
    if (fatalAfterWrite_ && onFatalShutdown_ && !fatalNotified_) {
        fatalNotified_ = true;
        onFatalShutdown_();
    }
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
