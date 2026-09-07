#pragma once

#include <memory>
#include <vector>

#include <boost/asio.hpp>

#include "config.hpp"
#include "message_codec.hpp"
#include "request_router.hpp"
#include "session.hpp"

namespace matching_engine {

// Владеет acceptor'ом, принимает соединения и создаёт по Session на
// каждое (REQ-NET-08, REQ-EXT-01: число соединений искусственно не
// ограничивается). Про устройство книги заявок ничего не знает — только
// про TCP и про то, что каждой сессии нужен MessageCodec и RequestRouter.
//
// Bind и listen выполняются синхронно внутри конструктора: тесты узнают
// фактический порт сразу после его вызова, не дожидаясь, пока
// io_context начнёт крутиться в отдельном потоке (план ДЗ-4, "Готовность
// сервера в тестах определяется синхронизацией"). Собственно приём
// соединений (async_accept) начинается только в start() — это разделение
// нужно, чтобы порядок запуска (REQ-NET-14) мог поставить start() строго
// последним шагом, уже после восстановления книги и прогрева кеша.
class Server {
public:
    Server(boost::asio::io_context& ioContext, const ServerConfig& config, RequestRouter& router);

    // Сообщает о готовности строкой "Listening on <адрес>:<порт>"
    // (REQ-NET-13) и начинает принимать соединения. Вызывающая сторона
    // отвечает за порядок: start() зовётся последним, после подключения к
    // БД, восстановления книги и прогрева кеша (REQ-NET-14) — сам Server
    // об этих шагах не знает и никак их не проверяет.
    void start();

    // Безопасен для вызова из другого потока (сигнальный поток задачи 08,
    // поток теста, крутящий io_context сервера): закрытие acceptor'а
    // переносится в io_context через post, а не выполняется напрямую —
    // acceptor не потокобезопасен сам по себе, но post в его собственный
    // io_context is thread-safe по контракту Asio.
    void stop();

    // Порт, на котором реально начал слушать acceptor — при config.port
    // == 0 операционная система выбирает его сама, и узнать номер можно
    // только после bind().
    unsigned short port() const noexcept { return port_; }

private:
    void doAccept();

    boost::asio::io_context& ioContext_;
    boost::asio::ip::tcp::acceptor acceptor_;
    MessageCodec codec_;
    RequestRouter& router_;
    std::string address_;
    unsigned short port_ = 0;

    // Реестр живых сессий слабыми ссылками (REQ-NET-08): на время жизни
    // сессий не влияет (только shared_from_this() в самой Session
    // управляет им), нужен будущему graceful shutdown задачи 08/11, чтобы
    // достучаться до активных соединений при остановке сервера.
    std::vector<std::weak_ptr<Session>> sessions_;
};

}
