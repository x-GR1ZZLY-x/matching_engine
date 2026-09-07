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
            if (!ec) {
                auto session = std::make_shared<Session>(std::move(socket), codec_, router_);

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
        });
}

}
