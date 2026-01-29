#pragma once
#include <expected>
#include <future>

#include "EnvelopeUtils.h"
#include "packet.pb.h"

enum SendError {
    INVALID_PEER,
};

class ITransportLayer {
public:
    virtual ~ITransportLayer() = default;

    virtual std::expected<std::future<std::string>, SendError> send_tcp_message(
        const std::string &peer,
        mesh::Envelope &envelope,
        std::optional<std::chrono::milliseconds> timeout = std::nullopt
    ) = 0;

    virtual std::expected<void, SendError> send_udp_message(
        const std::string &peer,
        mesh::RoutedPacket &pkt
    ) = 0;
};

