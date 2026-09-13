#include "client.hpp"

#include <array>
#include <memory>
#include <vector>

#include "exceptions.hpp"

namespace matching_engine {

Client::Client(std::size_t maxMessageSize, std::chrono::milliseconds timeout)
    : socket_(ioContext_), codec_(maxMessageSize), timeout_(timeout) {}

Client::~Client() {
    close();
}

void Client::close() {
    boost::system::error_code ignored;
    socket_.close(ignored);
}

void Client::connect(const std::string& host, unsigned short port) {
    boost::asio::ip::tcp::resolver resolver(ioContext_);

    // would_block — сентинел "операция ещё не завершилась": реальный
    // error_code от resolve/connect его никогда не примет, поэтому им
    // можно отличить настоящий таймаут (run_for вернулся, а результат так
    // и не пришёл) от штатного завершения с кодом success. Хранится в
    // shared_ptr, а не как локальная переменная, на которую замыкание
    // держит ссылку: если run_for вернётся по таймауту раньше, чем
    // завершится сама операция, обработчик может сработать позже — уже
    // после того, как этот стек-фрейм вернётся и бросит исключение, — и
    // тогда обращение к разрушенной локальной переменной было бы неопре-
    // делённым поведением.
    auto resultEc = std::make_shared<boost::system::error_code>(boost::asio::error::would_block);

    resolver.async_resolve(host, std::to_string(port),
        [this, resultEc](const boost::system::error_code& ec,
            const boost::asio::ip::tcp::resolver::results_type& endpoints) {
            if (ec) {
                *resultEc = ec;
                return;
            }
            boost::asio::async_connect(socket_, endpoints,
                [resultEc](const boost::system::error_code& connectEc,
                    const boost::asio::ip::tcp::endpoint&) { *resultEc = connectEc; });
        });

    ioContext_.restart();
    ioContext_.run_for(timeout_);

    if (*resultEc == boost::asio::error::would_block) {
        close();
        throw NetworkTimeoutError("Connect to " + host + ":" + std::to_string(port) + " timed out");
    }
    if (*resultEc) {
        throw NetworkError("Failed to connect to " + host + ":" + std::to_string(port) + ": " +
            resultEc->message());
    }
}

std::string Client::encodeFrame(const std::string& payload) const {
    return codec_.encode(payload);
}

void Client::sendRawBytes(const std::string& bytes) {
    auto resultEc = std::make_shared<boost::system::error_code>(boost::asio::error::would_block);

    // Буфер операции живёт в shared_ptr и захвачен в обработчик по тем же
    // причинам, что и resultEc: на пути таймаута этот метод бросает
    // исключение, и переданная по ссылке строка (в request() — временный
    // объект) может исчезнуть раньше, чем асинхронная запись, которую
    // close() ещё не успел отменить, закончит читать из неё.
    auto data = std::make_shared<std::string>(bytes);

    boost::asio::async_write(socket_, boost::asio::buffer(*data),
        [resultEc, data](const boost::system::error_code& ec, std::size_t /*transferred*/) {
            *resultEc = ec;
        });

    ioContext_.restart();
    ioContext_.run_for(timeout_);

    if (*resultEc == boost::asio::error::would_block) {
        close();
        throw NetworkTimeoutError("Write timed out");
    }
    if (*resultEc) {
        throw NetworkError(std::string("Write failed: ") + resultEc->message());
    }
}

void Client::readExact(boost::asio::mutable_buffer buffer) {
    auto resultEc = std::make_shared<boost::system::error_code>(boost::asio::error::would_block);

    // Сама операция читает в буфер, выделенный здесь и живущий в
    // shared_ptr, а не напрямую в память вызывающей стороны (header/payload
    // в receiveFrame): на пути таймаута этот метод бросает исключение, и
    // локальные объекты вызывающей стороны разрушатся раньше, чем
    // async_read, которую close() ещё не успел отменить, закончит писать в
    // буфер. Результат копируется в buffer только на успешном пути, до
    // возврата из функции.
    auto data = std::make_shared<std::vector<char>>(boost::asio::buffer_size(buffer));

    boost::asio::async_read(socket_, boost::asio::buffer(*data),
        [resultEc, data](const boost::system::error_code& ec, std::size_t /*transferred*/) {
            *resultEc = ec;
        });

    ioContext_.restart();
    ioContext_.run_for(timeout_);

    if (*resultEc == boost::asio::error::would_block) {
        close();
        throw NetworkTimeoutError("Read timed out");
    }
    if (*resultEc) {
        throw NetworkError(std::string("Read failed: ") + resultEc->message());
    }
    boost::asio::buffer_copy(buffer, boost::asio::buffer(*data));
}

std::string Client::receiveFrame() {
    std::array<char, kFrameHeaderSize> header{};
    readExact(boost::asio::buffer(header));

    // decodeHeader бросает MessageTooLargeError, если объявленный размер
    // больше предела этого клиента, — тому же MessageCodec, что и на
    // сервере, здесь просто взят другой лимит (см. докстроку класса).
    const std::uint32_t payloadSize = codec_.decodeHeader(header);

    std::string payload(payloadSize, '\0');
    if (payloadSize > 0) {
        readExact(boost::asio::buffer(payload));
    }
    return payload;
}

nlohmann::json Client::request(const nlohmann::json& requestJson) {
    sendRawBytes(encodeFrame(requestJson.dump()));
    const std::string response = receiveFrame();
    try {
        return nlohmann::json::parse(response);
    } catch (const nlohmann::json::parse_error& e) {
        throw NetworkError(std::string("Failed to parse response JSON: ") + e.what());
    }
}

}
