#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include <boost/asio.hpp>

#include "message_codec.hpp"
#include "request_router.hpp"

namespace matching_engine {

// Одно TCP-соединение (REQ-NET-05, REQ-NET-06): владеет сокетом, читает
// заголовок кадра, читает тело, передаёт полезную нагрузку RequestRouter,
// ставит ответ в очередь записи и читает следующий кадр. Про устройство
// книги заявок ничего не знает — этим занимается router_ (REQ-NET-11).
//
// Живёт в std::shared_ptr и наследует std::enable_shared_from_this
// (REQ-NET-07): каждый асинхронный вызов захватывает shared_from_this() в
// свой обработчик, поэтому объект не разрушается, пока для него есть
// незавершённая операция. Явного удаления сессий в проекте нет — это
// единственный механизм управления временем жизни соединения.
class Session : public std::enable_shared_from_this<Session> {
public:
    // codec копируется (у MessageCodec одно поле — верхняя граница
    // размера, — копия дешевле и безопаснее ссылки на объект, чьё время
    // жизни сессии не контролирует). router_ передаётся ссылкой: он
    // принадлежит Server и живёт весь срок работы сервера, дольше любой
    // отдельной сессии.
    Session(boost::asio::ip::tcp::socket socket, const MessageCodec& codec, RequestRouter& router);

    // Запускает первое чтение кадра. Не делается из конструктора: внутри
    // конструктора shared_from_this() ещё не работает — объектом пока не
    // управляет ни один shared_ptr.
    void start();

private:
    void readHeader();
    void readBody(std::uint32_t payloadSize);

    // Заголовок объявил размер больше maxMessageSize (REQ-PROTO-10) —
    // нарушение протокола, а не ошибка пользователя: отправляет один ответ
    // с кодом MESSAGE_TOO_LARGE и закрывает соединение вместо того, чтобы
    // продолжать читать поток, доверяя источнику, который контракт уже
    // нарушил.
    void closeAfterMessageTooLarge(const std::string& message);

    void enqueueResponse(const std::string& payload);
    void writeNext();
    void closeSocket();

    boost::asio::ip::tcp::socket socket_;
    MessageCodec codec_;
    RequestRouter& router_;

    std::array<char, kFrameHeaderSize> headerBuffer_{};
    std::string bodyBuffer_;

    // Очередь готовых кадров-ответов (REQ-NET-09): новая асинхронная
    // запись не начинается, пока не завершилась предыдущая на этом же
    // сокете — иначе два перекрывающихся async_write перемешали бы байты
    // двух ответов. Кадр живёт здесь же, в члене класса, а не в локальной
    // переменной обработчика — иначе async_write продолжал бы ссылаться на
    // память, уже освобождённую к моменту завершения записи.
    std::deque<std::string> writeQueue_;

    // Взводится closeAfterMessageTooLarge(): после того как очередь
    // опустеет (то есть системный ответ об ошибке гарантированно уйдёт
    // раньше самого закрытия), writeNext() закрывает сокет вместо того,
    // чтобы читать следующий кадр.
    bool closeAfterWrite_ = false;
};

}
