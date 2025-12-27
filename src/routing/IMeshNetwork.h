#pragma once
#include <expected>
#include <future>

#include "EnvelopeUtils.h"

enum SendError {
    INVALID_PEER,
};

class IMeshTransport {
public:
    virtual ~IMeshTransport() = default;

    virtual std::expected<std::future<std::string>, SendError> send_message(
        const std::string &peer,
        mesh::Envelope &envelope,
        std::optional<std::chrono::milliseconds> timeout = std::nullopt
    ) = 0;
};

class IMeshReceiver {
public:
    virtual ~IMeshReceiver() = default;

    virtual void on_packet_received(const mesh::Envelope &env) = 0;

    virtual void on_peer_connected(const std::string &peer) = 0;

    virtual void on_peer_disconnected(const std::string &peer) = 0;
};

