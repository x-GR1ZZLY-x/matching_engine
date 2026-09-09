#include <gtest/gtest.h>

#include <csignal>
#include <cstdlib>
#include <chrono>
#include <future>
#include <optional>
#include <pthread.h>
#include <thread>
#include <unistd.h>

#include "signal_handler.hpp"

using namespace matching_engine;

namespace {

// Восстанавливает маску сигналов процесса на выходе из области видимости
// (docs/task4/01-service-lifecycle.md, раздел 7.1): SignalHandler::blockSignals()
// меняет маску вызывающего потока, а маска — свойство процесса. Тесты этого
// файла регистрируются через gtest_discover_tests и в норме выполняются
// каждый в своём процессе, но восстановление всё равно нужно — тот же
// бинарник можно запустить и без ctest, со всеми тестами в одном процессе
// (docs/task4/01-service-lifecycle.md, раздел 7.1: "маска сигналов —
// свойство процесса, поэтому тест обязан восстановить её в исходное
// состояние").
class SignalMaskGuard {
public:
    SignalMaskGuard() {
        pthread_sigmask(SIG_SETMASK, nullptr, &original_);
    }
    ~SignalMaskGuard() {
        pthread_sigmask(SIG_SETMASK, &original_, nullptr);
    }

private:
    sigset_t original_{};
};

}

// Критерий 8 (docs/task4/01-service-lifecycle.md, раздел 7.1): обработчик
// остановки вызывается настоящим сигналом SIGTERM, посланным самому
// процессу, и выполняется в сигнальном потоке SignalHandler, а не в потоке
// теста. Ограничен по времени: future::wait_for с таймаутом вместо
// std::this_thread::sleep_for (REQ-TEST-09).
TEST(SignalHandlerTest, RealSigtermInvokesHandlerFromSignalThread) {
    SignalMaskGuard maskGuard;
    SignalHandler::blockSignals();

    std::promise<std::thread::id> invokedOn;
    std::future<std::thread::id> invokedOnFuture = invokedOn.get_future();

    // std::promise::set_value можно вызвать только один раз: обработчик
    // может сработать повторно (например, на SIGUSR1 в деструкторе ниже),
    // и к этому моменту future уже прочитан этим же тестом — второй вызов
    // до этой защёлки бросил бы std::future_error.
    bool fulfilled = false;
    // std::optional, а не объект на стеке напрямую: разрушение (SIGUSR1 +
    // join в деструкторе) ниже выполняется явно, во вспомогательном потоке
    // с ограничением по времени, а не неявно на выходе из области
    // видимости — иначе на join не было бы предела вообще (критерий 8
    // требует ограничения по времени для всего теста, а не только для
    // ожидания вызова обработчика).
    std::optional<SignalHandler> handler;
    handler.emplace([&invokedOn, &fulfilled] {
        if (!fulfilled) {
            fulfilled = true;
            invokedOn.set_value(std::this_thread::get_id());
        }
    });

    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);

    constexpr std::chrono::seconds kTimeout(5);
    ASSERT_EQ(invokedOnFuture.wait_for(kTimeout), std::future_status::ready)
        << "обработчик остановки не был вызван за отведённое время";

    // Сигнальный поток — не поток теста: настоящий обработчик сигнала в
    // проекте отсутствует вообще (sigwait исполняется в обычном потоке), и
    // это единственное наблюдаемое отличие "правильно" от "вызвали как
    // обычную функцию из текущего стека".
    EXPECT_NE(invokedOnFuture.get(), std::this_thread::get_id());

    // Разрушение SignalHandler (SIGUSR1 + join в деструкторе) — во
    // вспомогательном потоке, с ограничением по времени: тот же приём, что
    // и в network_tests.cpp, StopDrainsHangingSessionWithoutDeadlock. Дефект
    // ровно в этом механизме (например, забытый pthread_kill в деструкторе)
    // иначе дал бы не падение теста, а зависание на join() до
    // умолчательного предела ctest — 25 минут тишины вместо красного теста.
    std::promise<void> destroyed;
    std::future<void> destroyedFuture = destroyed.get_future();
    std::thread destroyer([&handler, &destroyed] {
        handler.reset();
        destroyed.set_value();
    });

    if (destroyedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "SignalHandler не разрушился (SIGUSR1 + join) за отведённое время";
        // Присоединять поток, застрявший на заблокированном join(), нельзя —
        // это и есть зависание, от которого тест обязан отличаться падением,
        // а не попыткой присоединить заблокированный поток (docs/task4/
        // 01-service-lifecycle.md, раздел 7.2, пункт 5).
        std::_Exit(1);
    }
    destroyer.join();
}
