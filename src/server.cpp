#include "server.hpp"

#include <algorithm>

#include "logger.hpp"

namespace matching_engine {

namespace {

// Интервал опроса реестра сессий во время остановки (см. scheduleDrainCheck
// ниже) — короткий по сравнению с shutdownTimeout_, поэтому штатная
// остановка (сессии закрылись за миллисекунды) обнаруживается быстро, а не
// только по истечении полного предельного времени.
constexpr std::chrono::milliseconds kDrainPollInterval(20);

}

Server::Server(boost::asio::io_context& ioContext, const ServerConfig& config,
    RequestRouter& router, std::chrono::seconds shutdownTimeout)
    : ioContext_(ioContext),
      acceptor_(ioContext),
      codec_(config.maxMessageSize),
      router_(router),
      address_(config.address),
      shutdownTimer_(ioContext),
      shutdownTimeout_(shutdownTimeout) {

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
        // Шаги 3-6 раздела 4.1 документа: закрыть acceptor, обойти реестр
        // живых сессий, запустить контроль предельного времени остановки.
        // После этого новых асинхронных операций никто не запускает —
        // начатые дорабатывают, и io_context.run() возвращается сама (шаг
        // 7), без принудительной остановки цикла событий.
        if (!stopping_) {
            // Строка в журнал — тоже только на первом входе: иначе оператор
            // не отличит "остановка началась дважды" от "остановка не
            // началась и её повторили".
            Logger::instance().info("Stopping server: closing acceptor and draining sessions");

            // Дедлайн — только на первом входе: повторный stop() (повторный
            // SIGTERM) переиспользует уже вычисленный shutdownDeadline_, а
            // не сдвигает предельное время вперёд.
            stopping_ = true;
            shutdownDeadline_ = std::chrono::steady_clock::now() + shutdownTimeout_;
        }

        boost::system::error_code ignored;
        acceptor_.close(ignored);

        // Реестр хранит только слабые ссылки — сессия, разрушившаяся сама
        // (клиент уже отключился), просто не пробуждается по expired()
        // weak_ptr, а не оставляет висящий указатель.
        for (const std::weak_ptr<Session>& weak : sessions_) {
            if (auto session = weak.lock()) {
                session->beginClose();
            }
        }

        scheduleDrainCheck(shutdownDeadline_);
    });
}

void Server::scheduleDrainCheck(std::chrono::steady_clock::time_point deadline) {
    // Та же очистка, что и в handleAccept(): сессии, уже закрывшиеся сами
    // (beginClose() закрыл сокет немедленно, либо запись давно закончилась),
    // выпадают из реестра сюда же, без отдельного прохода.
    sessions_.erase(
        std::remove_if(sessions_.begin(), sessions_.end(),
            [](const std::weak_ptr<Session>& weak) { return weak.expired(); }),
        sessions_.end());

    // Цепочка опроса живёт, пока в реестре есть сессии: как только он
    // опустеет, ветка ниже (sessions_.empty()) обрывает её без нового
    // async_wait — дальнейших проверок предельного времени больше не будет,
    // потому что другой работы в цикле не остаётся, и io_context.run()
    // вернётся сама (шаг 7 раздела 4.1 документа). Постоянно взведённый
    // таймер сам держал бы io_context и ломал бы это свойство. Предельное
    // время проверяется первым в каждом отдельном вызове этого метода — пока
    // цепочка ещё жива, страховка успевает сработать до того, как реестр
    // будет заново обойдён.
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        // Предельное время остановки исчерпано — сетевой поток
        // останавливает цикл событий принудительно, не дожидаясь
        // незавершённых операций (раздел 4.3 документа), независимо от
        // того, пуст ли в этот момент реестр сессий.
        Logger::instance().error(
            "Graceful shutdown deadline exceeded, forcing io_context to stop");
        forceStopped_ = true;
        ioContext_.stop();
        return;
    }

    if (sessions_.empty()) {
        // Ждать больше нечего: закрывать таймер повторно не нужно — он не
        // переставлен на новое ожидание, поэтому в io_context не остаётся
        // ни одной незавершённой операции, и run() вернётся сама (шаг 7
        // раздела 4.1 документа). Если бы shutdownTimer_ был один раз
        // запущен на весь shutdownTimeout_ и не отменялся, он сам оставался
        // бы для io_context незавершённой операцией до истечения полного
        // срока — штатная остановка (сессии закрылись за миллисекунды) всё
        // равно ждала бы отведённые секунды целиком.
        return;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    shutdownTimer_.expires_after(std::min(kDrainPollInterval, remaining));
    shutdownTimer_.async_wait([this, deadline](boost::system::error_code ec) {
        if (ec) {
            // Достижимо: повторный SIGTERM — это повторный stop(), который
            // безусловно заканчивается вызовом scheduleDrainCheck(); тот
            // вызывает shutdownTimer_.expires_after() для новой цепочки
            // опроса, а expires_after() отменяет ожидание, ещё стоявшее от
            // предыдущей, — её обработчик получает operation_aborted именно
            // здесь. Обрыв старой цепочки на этой ошибке не даёт двум
            // цепочкам одновременно перевзводить один и тот же таймер.
            return;
        }
        scheduleDrainCheck(deadline);
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
        // Верно в server_main.cpp и в обеих тестовых обёртках,
        // TestServer и ManualServer (tests/network_tests.cpp, порядок полей
        // там объявлен намеренно) — перестановка полей в любом из этих мест
        // молча сломает этот инвариант.
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

        // Соединение, принятое одновременно с решением об остановке
        // (этот обработчик успел завершиться успехом до выполнения
        // запощенного stop() — раздел 4.1 документа): реестр сессий stop()
        // уже обошёл, второго обхода не будет, поэтому сессия сразу
        // получает beginClose() вместо start() (REQ-EXT-08).
        if (stopping_) {
            session->beginClose();
        } else {
            session->start();
        }
    } else if (ec != boost::asio::error::operation_aborted) {
        // Устойчивая ошибка accept (например, исчерпаны дескрипторы)
        // иначе привела бы к холостому циклу doAccept() без единой
        // строки в журнале.
        Logger::instance().error(std::string("Accept failed: ") + ec.message());
    }

    // Признак, по которому решается, принимать ли дальше, — открыт ли
    // acceptor, а не успешность конкретной попытки. Закрытый stop()'ом
    // acceptor отдаёт async_accept немедленным bad_descriptor, а не
    // operation_aborted (последним отменяется только попытка, уже стоявшая
    // в очереди на момент close()), поэтому перевзведение без разбора
    // причины даёт холостой цикл на 100% CPU. Проверять при этом успех
    // нельзя: accept штатно завершается ошибкой и на живом acceptor'е —
    // ECONNABORTED, когда клиент разорвал соединение между connect и
    // accept, — и сервер, прекративший приём после одной такой ошибки,
    // молча перестал бы обслуживать новые подключения (REQ-NET-01,
    // REQ-EXT-01).
    if (acceptor_.is_open()) {
        doAccept();
    }
}

}
