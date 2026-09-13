#include "message_codec.hpp"
#include "exceptions.hpp"

#include <arpa/inet.h>
#include <cstring>

namespace matching_engine {

// Заголовок кадра — это ровно один сетевой uint32_t (docs/task4/
// 02-network-protocol.md, раздел 1). Правка kFrameHeaderSize без синхронной
// правки этого инварианта иначе прошла бы молча: memcpy ниже писал бы не то
// число байт в 4-байтовую переменную на стеке.
static_assert(kFrameHeaderSize == sizeof(std::uint32_t),
    "kFrameHeaderSize must match sizeof(std::uint32_t)");

MessageCodec::MessageCodec(std::size_t maxPayloadSize) : maxPayloadSize_(maxPayloadSize) {}

std::string MessageCodec::encode(const std::string& payload) const {
    if(payload.size() > maxPayloadSize_){
        throw MessageTooLargeError(
            "Payload size " + std::to_string(payload.size()) +
            " exceeds maximum message size " + std::to_string(maxPayloadSize_));
    }

    const std::uint32_t payloadSize = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t networkSize = htonl(payloadSize);

    std::string frame;
    frame.reserve(kFrameHeaderSize + payload.size());
    frame.append(reinterpret_cast<const char*>(&networkSize), kFrameHeaderSize);
    frame.append(payload);
    return frame;
}

std::uint32_t MessageCodec::decodeHeader(const std::array<char, kFrameHeaderSize>& header) const {
    std::uint32_t networkSize = 0;
    std::memcpy(&networkSize, header.data(), kFrameHeaderSize);
    const std::uint32_t payloadSize = ntohl(networkSize);

    if(static_cast<std::size_t>(payloadSize) > maxPayloadSize_){
        throw MessageTooLargeError(
            "Payload size " + std::to_string(payloadSize) +
            " exceeds maximum message size " + std::to_string(maxPayloadSize_));
    }
    return payloadSize;
}

}
