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

    // Безопасен для вызова из другого потока (сигнальный поток,
    // обрабатывающий SIGTERM/SIGINT согласно REQ-THR-04, поток теста,
    // крутящий io_context сервера): закрытие acceptor'а
    // переносится в io_context через post, а не выполняется напрямую —
    // acceptor не потокобезопасен сам по себе, но post в его собственный
    // io_context is thread-safe по контракту Asio.
    void stop();

    // Порт, на котором реально начал слушать acceptor — при config.port
    // == 0 операционная система выбирает его сама, и узнать номер можно
    // только после bind().
    unsigned short port() const noexcept { return port_; }

    // Взводится, когда одна из сессий сообщила о сбое сохранения в БД
    // (PersistenceError, docs/task4/02-network-protocol.md, раздел 3.5):
    // книга в памяти разошлась с хранилищем, поэтому вызывающая сторона
    // (server_main.cpp) обязана завершить процесс с кодом 1 после того,
    // как io_context.run() вернётся.
    bool hadFatalError() const noexcept { return fatalError_; }

private:
    void doAccept();

    // Тело обработчика async_accept: вынесено из лямбды doAccept() отдельным
    // методом, чтобы сама лямбда оставалась короткой и не прятала внутри
    // себя ветвление, от которого зависит взведение onFatalShutdown (см.
    // ниже). Вызывается только из doAccept().
    void handleAccept(boost::system::error_code ec, boost::asio::ip::tcp::socket socket);

    boost::asio::io_context& ioContext_;
    boost::asio::ip::tcp::acceptor acceptor_;
    MessageCodec codec_;
    RequestRouter& router_;
    std::string address_;
    unsigned short port_ = 0;
    bool fatalError_ = false;

    // Реестр живых сессий слабыми ссылками (REQ-NET-08): на время жизни
    // сессий не влияет (только shared_from_this() в самой Session
    // управляет им), нужен будущему graceful drain (REQ-THR-11, REQ-EXT-08),
    // чтобы достучаться до активных соединений при остановке сервера.
    std::vector<std::weak_ptr<Session>> sessions_;
};

}
