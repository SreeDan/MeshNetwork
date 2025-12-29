#pragma once
#include <expected>
#include <future>

#include "EnvelopeUtils.h"

enum SendError {
    INVALID_PEER,
};

class ITransportLayer {
public:
    virtual ~ITransportLayer() = default;

    virtual std::expected<std::future<std::string>, SendError> send_message(
        const std::string &peer,
        mesh::Envelope &envelope,
        std::optional<std::chrono::milliseconds> timeout = std::nullopt
    ) = 0;
};

