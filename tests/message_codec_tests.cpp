#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include "exceptions.hpp"
#include "message_codec.hpp"

using namespace matching_engine;

namespace {

// Строит 4 байта сетевого порядка сдвигами, независимо от MessageCodec.
// Тестам, которым нужно подать заголовок с произвольным объявленным
// размером напрямую (в обход MessageCodec::encode), нужен способ собрать
// такой заголовок, не полагаясь на код, который сам же проверяется.
std::array<char, kFrameHeaderSize> bigEndianHeader(std::uint32_t value){
    std::array<char, kFrameHeaderSize> header{};
    header[0] = static_cast<char>((value >> 24) & 0xFF);
    header[1] = static_cast<char>((value >> 16) & 0xFF);
    header[2] = static_cast<char>((value >> 8) & 0xFF);
    header[3] = static_cast<char>(value & 0xFF);
    return header;
}

}

// Критерий 1: кодирование и декодирование симметричны побайтово. Тело
// содержит внутренний нулевой байт: для кодека тело — непрозрачные байты, а
// не C-строка, и реализация, копирующая тело как C-строку (strcpy-подобным
// путём), должна проиграть это сравнение std::string целиком.
TEST(MessageCodecTest, EncodeDecodeRoundTripIsByteForByteSymmetric){
    const MessageCodec codec(1024);
    std::string payload = "{\"type\":\"PING\"}";
    payload.push_back('\0');
    payload += "TAIL";

    const std::string frame = codec.encode(payload);
    ASSERT_EQ(frame.size(), kFrameHeaderSize + payload.size());

    std::array<char, kFrameHeaderSize> header{};
    std::copy(frame.begin(), frame.begin() + kFrameHeaderSize, header.begin());

    const std::uint32_t decodedSize = codec.decodeHeader(header);
    EXPECT_EQ(decodedSize, payload.size());

    const std::string decodedPayload = frame.substr(kFrameHeaderSize);
    EXPECT_EQ(decodedPayload, payload);
}

// Критерий 2: заголовок содержит длину именно в сетевом порядке байт.
// Проверяются сырые байты кадра, а не результат обратного преобразования.
// Длина выбрана так, чтобы значимыми были минимум два байта из четырёх
// (258 = 0x00000102): на длине 1 три байта из четырёх нулевые, и реализация,
// записывающая только младший байт без htonl, тоже дала бы 0x00 0x00 0x00
// 0x01 — тест такую реализацию не поймал бы.
TEST(MessageCodecTest, HeaderEncodesLengthInNetworkByteOrder){
    const MessageCodec codec(1024);
    const std::string payload(258, 'X');

    const std::string frame = codec.encode(payload);
    ASSERT_EQ(frame.size(), kFrameHeaderSize + payload.size());

    EXPECT_EQ(static_cast<unsigned char>(frame[0]), 0x00);
    EXPECT_EQ(static_cast<unsigned char>(frame[1]), 0x00);
    EXPECT_EQ(static_cast<unsigned char>(frame[2]), 0x01);
    EXPECT_EQ(static_cast<unsigned char>(frame[3]), 0x02);
}

// encode обязан отвергать тело больше maxPayloadSize_ так же, как
// decodeHeader отвергает заголовок, объявляющий такой размер: кодек один и
// тот же на обеих сторонах, и ответ длиннее максимума не должен уходить в
// сеть — на другом конце это выглядело бы как нарушение протокола, которое
// вызвал не получатель.
TEST(MessageCodecTest, EncodeRejectsPayloadAboveMaximum){
    const MessageCodec codec(4);
    const std::string payload(5, 'A');

    EXPECT_THROW(codec.encode(payload), MessageTooLargeError);
}

// Кадр с пустым телом — ровно четыре нулевых байта заголовка и ничего
// больше: решение по нулевой длине зафиксировано в разделе 1.2 документа
// протокола, и сборка кадра не должна добавлять байты тела для пустой
// строки.
TEST(MessageCodecTest, EncodeEmptyPayloadProducesFourZeroBytes){
    const MessageCodec codec(1024);

    const std::string frame = codec.encode("");

    ASSERT_EQ(frame.size(), kFrameHeaderSize);
    for(char byte : frame){
        EXPECT_EQ(static_cast<unsigned char>(byte), 0x00);
    }
}

// Критерий 3: заголовок, объявляющий размер больше максимального,
// отвергается. decodeHeader не выделяет память под тело сам по себе — тело
// в кодек вообще не передаётся, это проверяется чтением кода.
TEST(MessageCodecTest, RejectsSizeAboveMaximum){
    const MessageCodec codec(1024);
    const auto header = bigEndianHeader(2000);

    EXPECT_THROW(codec.decodeHeader(header), MessageTooLargeError);
}

// Критерий 4: заявленный размер порядка 2 ГБ приводит только к отказу, а не
// к выделению памяти или аварийному завершению.
TEST(MessageCodecTest, RejectsSizeNearTwoGigabytesWithoutCrashing){
    const MessageCodec codec(1024);
    const auto header = bigEndianHeader(2147483648u);

    EXPECT_THROW(codec.decodeHeader(header), MessageTooLargeError);
}

// Значение вплотную к пределу uint32_t: проверка границ буфера не должна
// переполниться и ложно принять кадр как допустимый.
TEST(MessageCodecTest, RejectsSizeNearUint32MaxWithoutOverflow){
    const MessageCodec codec(1024);
    const auto header = bigEndianHeader(0xFFFFFFFEu);

    EXPECT_THROW(codec.decodeHeader(header), MessageTooLargeError);
}

// Критерий 5: граничные значения — размер, равный максимальному, принят;
// размер на единицу больше — отвергнут.
TEST(MessageCodecTest, AcceptsExactMaximumAndRejectsOneAboveIt){
    const MessageCodec codec(100);

    const auto atMax = bigEndianHeader(100);
    EXPECT_EQ(codec.decodeHeader(atMax), 100u);

    const auto aboveMax = bigEndianHeader(101);
    EXPECT_THROW(codec.decodeHeader(aboveMax), MessageTooLargeError);
}

// Критерий 6: нулевой размер полезной нагрузки корректен на уровне кадра
// (docs/task4/02-network-protocol.md, раздел 1.2) — decodeHeader его не
// отвергает, решение о пустой строке принимается позже, при разборе JSON.
TEST(MessageCodecTest, ZeroPayloadSizeIsAcceptedAtFrameLevel){
    const MessageCodec codec(1024);
    const auto header = bigEndianHeader(0);

    EXPECT_EQ(codec.decodeHeader(header), 0u);
}
