#include "signal_handler.hpp"

#include <csignal>
#include <exception>
#include <pthread.h>

#include "logger.hpp"

namespace matching_engine {

namespace {

sigset_t stopSignalSet() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);
    return set;
}

}

bool SignalHandler::blockSignals() {
    sigset_t set = stopSignalSet();
    // pthread_sigmask, а не sigprocmask (раздел 2.1 документа): маска
    // ставится для конкретного потока (главного) и наследуется потоками,
    // созданными после этого вызова, а не только процессом в целом.
    const int rc = pthread_sigmask(SIG_BLOCK, &set, nullptr);
    if (rc != 0) {
        // На этой маске держится вся схема остановки: без неё SIGTERM,
        // пришедший во время восстановления книги, убьёт процесс действием
        // по умолчанию вместо того, чтобы остаться в очереди отложенных
        // сигналов до первого sigwait(). Вызывающая сторона обязана
        // завершить запуск с кодом 1, а не продолжать со снятой защитой.
        Logger::instance().error(
            "Failed to block signals: pthread_sigmask returned " + std::to_string(rc));
        return false;
    }
    return true;
}

SignalHandler::SignalHandler(std::function<void()> onStop, std::function<void()> onFailure)
    : onStop_(std::move(onStop)), onFailure_(std::move(onFailure)), thread_([this] { run(); }) {}

SignalHandler::~SignalHandler() {
    // Стандартный интерфейс std::thread не даёт способа послать сигнал
    // конкретному потоку, поэтому используется pthread_kill; native_handle()
    // на POSIX — это pthread_t. Сигнальный поток может в этот момент стоять
    // на sigwait() (штатно) или уже выполнять обработчик остановки от
    // настоящего SIGTERM/SIGINT — в обоих случаях SIGUSR1 либо прерывает
    // ожидание, либо останется в очереди сигналов потока и будет получен
    // следующим sigwait().
    const int rc = pthread_kill(thread_.native_handle(), SIGUSR1);
    if (rc != 0) {
        // Недоставленный SIGUSR1 означает, что join() ниже не вернётся
        // никогда — поток продолжит стоять на sigwait(). Продолжить всё
        // равно необходимо (это деструктор), но строка в журнале —
        // единственный шанс узнать причину зависания.
        Logger::instance().error(
            "Failed to wake signal thread: pthread_kill returned " + std::to_string(rc));
    }
    thread_.join();
}

void SignalHandler::run() {
    // Тело потока целиком обёрнуто в перехват (REQ-THR-12, docs/task4/
    // 01-service-lifecycle.md, раздел 4.4): исключение, покинувшее поток, —
    // это std::terminate без внятного сообщения, худший из способов узнать
    // о дефекте.
    try {
        sigset_t set = stopSignalSet();
        while (true) {
            int signalNumber = 0;
            const int rc = sigwait(&set, &signalNumber);
            if (rc != 0) {
                Logger::instance().error("Signal thread: sigwait failed");
                // Все три сигнала заблокированы маской, а sigwait — единственное
                // место, где они снимаются: без вызова обработчика остановки
                // SIGTERM навсегда остался бы в очереди отложенных, и служба
                // была бы убита по таймауту вместо штатной остановки (раздел
                // 4.4 документа). onFailure_ взводит наблюдаемый снаружи
                // признак отказа (REQ-THR-12) — иначе такая остановка
                // неотличима от штатной по SIGTERM и даёт код 0. Вложенный
                // try — чтобы исключение из onFailure_/onStop_ не покинуло
                // поток.
                try {
                    if (onFailure_) {
                        onFailure_();
                    }
                    if (onStop_) {
                        onStop_();
                    }
                } catch (...) {
                }
                return;
            }

            // Раздел 4.1 документа, шаг 1: строка о полученном сигнале —
            // только для SIGTERM/SIGINT. SIGUSR1 — внутренний сигнал
            // пробуждения (штатно посылается самим процессом после
            // возврата из io_context.run()); отдельная строка о его
            // получении добавила бы шум без диагностической пользы.
            if (signalNumber == SIGTERM) {
                Logger::instance().info("Received SIGTERM, starting shutdown");
            } else if (signalNumber == SIGINT) {
                Logger::instance().info("Received SIGINT, starting shutdown");
            }

            // Различение "остановка" / "пробуждение" сигнальный поток
            // делает по номеру сигнала, который вернул sigwait, а не по
            // флагу-переменной (раздел 2.4 документа) — разделяемых
            // изменяемых данных между потоками в этой схеме нет.
            if (onStop_) {
                onStop_();
            }

            if (signalNumber == SIGUSR1) {
                return;
            }
        }
    } catch (const std::exception& e) {
        // Тело catch целиком обёрнуто во вложенный try (а не только onStop_
        // ниже): самый правдоподобный источник повторного исключения в этом
        // потоке — сам Logger (сбой форматирования в spdlog, bad_alloc), и
        // если бы первый же вызов Logger::instance().error() здесь остался
        // незащищённым, его исключение покинуло бы run() и дало бы
        // std::terminate без единой строки в журнале (REQ-THR-12).
        try {
            Logger::instance().error(std::string("Signal thread error: ") + e.what());
            // Тот же довод, что и у sigwait-ветки выше: без вызова
            // обработчика остановки поток выходит, а SIGTERM снять больше
            // некому. onFailure_ взводит наблюдаемый снаружи признак отказа.
            if (onFailure_) {
                onFailure_();
            }
            if (onStop_) {
                onStop_();
            }
        } catch (...) {
        }
    } catch (...) {
        try {
            Logger::instance().error("Signal thread error: unknown exception");
            if (onFailure_) {
                onFailure_();
            }
            if (onStop_) {
                onStop_();
            }
        } catch (...) {
        }
    }
}

}
