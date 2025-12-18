#pragma once
#include <memory>

#include "messages.pb.h"

class RpcConnection;

class IMessageHandler {
public:
    virtual ~IMessageHandler() = default;

    virtual void handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) = 0;
};
