#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "message_codec.hpp"

namespace matching_engine {

// Синхронный сетевой клиент (REQ-CLI-04) поверх асинхронного Boost.Asio:
// используется тестами уже в этой задаче и программой
// matching-engine-client в задаче 06 — библиотека одна, второго клиента,
// проверяющего сам себя, в тестах нет.
//
// Каждая операция ограничена по времени изнутри: это фасад над
// async_connect/async_read/async_write с io_context::run_for(timeout).
// Сервер, который не отвечает, приводит к NetworkError по истечении
// таймаута, а не к зависанию вызывающего потока — иначе сетевые тесты не
// смогли бы гарантированно падать, а не висеть.
//
// Кодек, которым Client кодирует и декодирует кадры, строится с тем
// maxMessageSize, что передан в конструктор, — независимо от предела
// сервера. Тест превышения размера намеренно создаёт клиента с бОльшим
// пределом, чем у сервера, и честно отправляет кадр, который для сервера
// слишком велик (docs/task4/02-network-protocol.md, раздел 1.1).
class Client {
public:
    explicit Client(std::size_t maxMessageSize,
        std::chrono::milliseconds timeout = std::chrono::seconds(2));

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client();

    // Резолвит host:port и подключается. Бросает NetworkError при отказе
    // соединения или истечении таймаута.
    void connect(const std::string& host, unsigned short port);

    // Удобство для типового сценария "один запрос — один ответ": кодирует
    // requestJson, отправляет одним кадром, дожидается и разбирает ответ.
    nlohmann::json request(const nlohmann::json& requestJson);

    // Ниже — примитивы для тестов протокола (фрагментация, склейка кадров,
    // превышение размера), которым нужно управлять байтами на проводе
    // напрямую, а не только целыми запросами. Кадр всё равно собирается
    // кодеком, а не вручную — манипуляции с байтами вне MessageCodec
    // запрещены (REQ-PROTO-05).
    std::string encodeFrame(const std::string& payload) const;
    void sendRawBytes(const std::string& bytes);
    std::string receiveFrame();

    // Закрывает сокет, если он ещё открыт. Тестам нужен явный вызов, чтобы
    // проверить поведение сервера при обрыве соединения клиентом, не
    // дожидаясь конца области видимости объекта.
    void close();

private:
    void readExact(boost::asio::mutable_buffer buffer);

    boost::asio::io_context ioContext_;
    boost::asio::ip::tcp::socket socket_;
    MessageCodec codec_;
    std::chrono::milliseconds timeout_;
};

}
