#pragma once
#include <string>

#include "envelope.pb.h"

class IMessageSink {
public:
    virtual ~IMessageSink() = default;

    virtual void push_data_bytes(const std::string &from_peer, const std::string &payload_bytes) = 0;

    virtual void on_peer_connected(const std::string &peer_id) = 0;

    virtual void on_peer_disconnected(const std::string &peer_id) = 0;
};
