#pragma once
#include <memory>

#include "envelope.pb.h"

class RpcConnection;

class IRpcMessageHandler {
public:
    virtual ~IRpcMessageHandler() = default;

    virtual void handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) = 0;
};
