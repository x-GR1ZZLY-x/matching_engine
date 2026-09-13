#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <future>
#include <optional>
#include <pthread.h>
#include <stdexcept>
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

// Сценарий из docs/task4/01-service-lifecycle.md, раздел 7.1: обработчик
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
    // видимости — иначе на join не было бы предела вообще (тест должен быть
    // ограничен по времени целиком, а не только ожидание вызова обработчика).
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
        std::fflush(nullptr);
        std::_Exit(1);
    }
    destroyer.join();
}

// Тот же сценарий, что и выше, но SIGINT вместо SIGTERM — отдельная ветка
// в run() (Logger::instance().info("Received SIGINT...")), которую
// предыдущий тест не задевает вовсе: SIGTERM и SIGINT логируются разными
// строками, хотя оба приводят к одному и тому же вызову onStop_. Если бы
// SIGINT забыли добавить в маску (stopSignalSet()) или в развилку по
// signalNumber, этот тест повис бы или не дождался вызова обработчика.
TEST(SignalHandlerTest, RealSigintInvokesHandler) {
    SignalMaskGuard maskGuard;
    SignalHandler::blockSignals();

    std::promise<void> invoked;
    std::future<void> invokedFuture = invoked.get_future();
    bool fulfilled = false;

    std::optional<SignalHandler> handler;
    handler.emplace([&invoked, &fulfilled] {
        if (!fulfilled) {
            fulfilled = true;
            invoked.set_value();
        }
    });

    ASSERT_EQ(::kill(::getpid(), SIGINT), 0);

    constexpr std::chrono::seconds kTimeout(5);
    ASSERT_EQ(invokedFuture.wait_for(kTimeout), std::future_status::ready)
        << "обработчик остановки не был вызван по SIGINT за отведённое время";

    std::promise<void> destroyed;
    std::future<void> destroyedFuture = destroyed.get_future();
    std::thread destroyer([&handler, &destroyed] {
        handler.reset();
        destroyed.set_value();
    });
    if (destroyedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "SignalHandler не разрушился за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    destroyer.join();
}

// REQ-THR-12: исключение, брошенное из onStop_ на штатном пути (после
// настоящего SIGTERM), обязано быть перехвачено внутри run() (catch(const
// std::exception&)), а не покинуть поток. Перехват сам вызывает onFailure_,
// затем повторно onStop_ — на этот раз без исключения, чтобы тест мог
// зафиксировать, что до этой точки дело дошло. Без этой защиты поток
// завершился бы std::terminate, и деструктор SignalHandler завис бы на
// join() навсегда — тест обязан поймать именно это, а не просто "не упал".
TEST(SignalHandlerTest, ExceptionFromOnStopIsCaughtAndReportedViaOnFailure) {
    SignalMaskGuard maskGuard;
    SignalHandler::blockSignals();

    std::promise<void> failureInvoked;
    std::future<void> failureFuture = failureInvoked.get_future();
    bool failureFulfilled = false;
    int stopCallCount = 0;

    std::optional<SignalHandler> handler;
    handler.emplace(
        [&stopCallCount] {
            ++stopCallCount;
            if (stopCallCount == 1) {
                throw std::runtime_error("boom from onStop_");
            }
        },
        [&failureInvoked, &failureFulfilled] {
            if (!failureFulfilled) {
                failureFulfilled = true;
                failureInvoked.set_value();
            }
        });

    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);

    constexpr std::chrono::seconds kTimeout(5);
    ASSERT_EQ(failureFuture.wait_for(kTimeout), std::future_status::ready)
        << "onFailure_ не был вызван после исключения из onStop_ за отведённое время";

    std::promise<void> destroyed;
    std::future<void> destroyedFuture = destroyed.get_future();
    std::thread destroyer([&handler, &destroyed] {
        handler.reset();
        destroyed.set_value();
    });
    if (destroyedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "SignalHandler не разрушился за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    destroyer.join();
}

// Та же гарантия REQ-THR-12, но для исключения, НЕ производного от
// std::exception (catch(...) в run(), отдельная от catch(const
// std::exception&) выше ветка).
TEST(SignalHandlerTest, NonStdExceptionFromOnStopIsCaughtByCatchAll) {
    SignalMaskGuard maskGuard;
    SignalHandler::blockSignals();

    std::promise<void> failureInvoked;
    std::future<void> failureFuture = failureInvoked.get_future();
    bool failureFulfilled = false;
    int stopCallCount = 0;

    std::optional<SignalHandler> handler;
    handler.emplace(
        [&stopCallCount] {
            ++stopCallCount;
            if (stopCallCount == 1) {
                throw 42; // не std::exception — должно поймать catch(...).
            }
        },
        [&failureInvoked, &failureFulfilled] {
            if (!failureFulfilled) {
                failureFulfilled = true;
                failureInvoked.set_value();
            }
        });

    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);

    constexpr std::chrono::seconds kTimeout(5);
    ASSERT_EQ(failureFuture.wait_for(kTimeout), std::future_status::ready)
        << "onFailure_ не был вызван после не-std::exception из onStop_ за отведённое время";

    std::promise<void> destroyed;
    std::future<void> destroyedFuture = destroyed.get_future();
    std::thread destroyer([&handler, &destroyed] {
        handler.reset();
        destroyed.set_value();
    });
    if (destroyedFuture.wait_for(kTimeout) != std::future_status::ready) {
        ADD_FAILURE() << "SignalHandler не разрушился за отведённое время";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    destroyer.join();
}
