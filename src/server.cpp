#include "server.hpp"

#include <algorithm>

#include "logger.hpp"

namespace matching_engine {

Server::Server(boost::asio::io_context& ioContext, const ServerConfig& config,
    RequestRouter& router)
    : ioContext_(ioContext),
      acceptor_(ioContext),
      codec_(config.maxMessageSize),
      router_(router),
      address_(config.address) {

    // Bind и listen — синхронно, до того как io_context начнёт крутиться в
    // отдельном потоке. Это единственный способ узнать фактический порт
    // сразу после конструктора, когда config.port == 0.
    const boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::make_address(config.address), static_cast<unsigned short>(config.port));

    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    port_ = acceptor_.local_endpoint().port();
}

void Server::start() {
    Logger::instance().info("Listening on " + address_ + ":" + std::to_string(port_));
    doAccept();
}

void Server::stop() {
    boost::asio::post(ioContext_, [this] {
        boost::system::error_code ignored;
        acceptor_.close(ignored);
    });
}

void Server::doAccept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            handleAccept(ec, std::move(socket));
        });
}

void Server::handleAccept(boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
    if (!ec) {
        // Сбой сохранения в БД (PersistenceError) касается не
        // одного соединения, а всего сервиса (docs/task4/
        // 02-network-protocol.md, раздел 3.5): после того как
        // сессия отправит клиенту INTERNAL_ERROR и закроет свой
        // сокет, этот обработчик закрывает acceptor и
        // останавливает io_context — он уже выполняется внутри
        // io_context (это completion handler самой сессии), поэтому
        // прямой вызов безопасен и без post().
        // Лямбда захватывает this, а не shared/weak-ссылку — это безопасно
        // только потому, что сессии переживают Server. Session хранит эту
        // лямбду в std::function и держит её живой, пока у сессии есть
        // незавершённые асинхронные обработчики (REQ-NET-07), то есть
        // потенциально дольше самого Server. Инвариант, на котором это
        // держится: Server обязан разрушаться раньше io_context, которым он
        // владеет косвенно через acceptor_/ioContext_, — тогда неисполненные
        // обработчики (а вместе с ними и захваченные лямбды) уничтожаются
        // вместе с io_context, а не вызываются на уже разрушенном Server.
        // Верно и в server_main.cpp, и в TestServer (tests/network_tests.cpp,
        // порядок полей там объявлен намеренно) — перестановка полей в любом
        // из двух мест молча сломает этот инвариант.
        auto session = std::make_shared<Session>(std::move(socket), codec_, router_,
            [this] {
                fatalError_ = true;
                boost::system::error_code ignored;
                acceptor_.close(ignored);
                ioContext_.stop();
            });

        // Реестр хранит только слабые ссылки — удаление умерших
        // записей здесь же, а не отдельным фоновым проходом:
        // проще, и растёт реестр только на живые соединения.
        sessions_.erase(
            std::remove_if(sessions_.begin(), sessions_.end(),
                [](const std::weak_ptr<Session>& weak) { return weak.expired(); }),
            sessions_.end());
        sessions_.push_back(session);

        session->start();
    } else if (ec != boost::asio::error::operation_aborted) {
        // Устойчивая ошибка accept (например, исчерпаны дескрипторы)
        // иначе привела бы к холостому циклу doAccept() без единой
        // строки в журнале.
        Logger::instance().error(std::string("Accept failed: ") + ec.message());
    }

    // ec == operation_aborted означает, что acceptor закрыт
    // (stop()) — новых соединений больше не принимаем, уже
    // открытые сессии продолжают работу самостоятельно.
    if (ec != boost::asio::error::operation_aborted) {
        doAccept();
    }
}

}
