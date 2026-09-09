#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
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
    // отдельной сессии. onFatalShutdown вызывается не более одного раза за
    // время жизни сессии (см. notifyFatalShutdown()/fatalNotified_ ниже —
    // при конвейерной обработке несколько точек кода готовы его вызвать на
    // одном и том же фатальном сбое), когда ответ, построенный после сбоя
    // сохранения в БД (PersistenceError), гарантированно ушёл клиенту либо
    // было решено, что отправлять уже нечего, — Server передаёт сюда
    // обработчик, закрывающий acceptor и останавливающий io_context
    // (docs/task4/02-network-protocol.md, раздел 3.5). Пустой по
    // умолчанию: в тестах, которые создают Session без Server, сбоев
    // сохранения не бывает.
    Session(boost::asio::ip::tcp::socket socket, const MessageCodec& codec, RequestRouter& router,
        std::function<void()> onFatalShutdown = {});

    // Запускает первое чтение кадра. Не делается из конструктора: внутри
    // конструктора shared_from_this() ещё не работает — объектом пока не
    // управляет ни один shared_ptr.
    void start();

    // Начинает закрытие сессии при остановке сервера (docs/task4/
    // 01-service-lifecycle.md, раздел 4.1, шаги 5-6; REQ-EXT-08): если
    // очередь записи пуста — закрывает сокет немедленно, иначе взводит уже
    // существующий признак "закрыться после записи" — тот же путь, которым
    // сессия закрывается после MESSAGE_TOO_LARGE (closeAfterMessageTooLarge)
    // выше: собственной логики закрытия здесь не заводится, а очередь
    // (writeNext()) сама закроет сокет, когда допишет накопленный ответ.
    void beginClose();

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

    // Вызывает onFatalShutdown_ ровно один раз (fatalNotified_ — защёлка):
    // несколько точек кода взводят fatalAfterWrite_ и готовы его позвать
    // (writeNext() дважды, closeAfterMessageTooLarge() и readBody() дважды),
    // и при конвейерной обработке запросов может сработать больше одной из
    // них на одном и том же фатальном сбое — см. комментарий у
    // fatalNotified_.
    void notifyFatalShutdown();

    boost::asio::ip::tcp::socket socket_;
    MessageCodec codec_;
    RequestRouter& router_;
    std::function<void()> onFatalShutdown_;

    std::array<char, kFrameHeaderSize> headerBuffer_{};
    std::string bodyBuffer_;

    // Очередь готовых кадров-ответов (REQ-NET-09): новая асинхронная
    // запись не начинается, пока не завершилась предыдущая на этом же
    // сокете — иначе два перекрывающихся async_write перемешали бы байты
    // двух ответов. Кадр живёт здесь же, в члене класса, а не в локальной
    // переменной обработчика — иначе async_write продолжал бы ссылаться на
    // память, уже освобождённую к моменту завершения записи.
    std::deque<std::string> writeQueue_;

    // Взводится closeAfterMessageTooLarge() и обработчиком PersistenceError
    // в readBody(): после того как очередь опустеет (то есть системный
    // ответ об ошибке гарантированно уйдёт раньше самого закрытия),
    // writeNext() закрывает сокет вместо того, чтобы читать следующий
    // кадр.
    bool closeAfterWrite_ = false;

    // Взводится вместе с closeAfterWrite_ только на пути PersistenceError:
    // после того как сокет закрыт, writeNext() зовёт onFatalShutdown_ —
    // сбой сохранения касается не только этого соединения, а всего
    // сервиса.
    bool fatalAfterWrite_ = false;

    // Защёлка notifyFatalShutdown(): при конвейерной обработке одно и то же
    // соединение может закрыться синхронно из readBody()/
    // closeAfterMessageTooLarge() (когда даже короткий ответ об ошибке не
    // помещается в лимит) и следом асинхронно — из writeNext(), когда уже
    // запущенная запись более раннего ответа на этом же, теперь закрытом,
    // сокете завершится ошибкой. Без этого флага onFatalShutdown_ (который
    // останавливает io_context всего сервиса, а не одно соединение) мог бы
    // вызваться дважды.
    bool fatalNotified_ = false;
};

}
