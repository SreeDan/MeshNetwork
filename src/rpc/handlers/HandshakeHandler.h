#pragma once
#include "IRpcMessageHandler.h"

class RpcConnection;

class HandshakeHandler : public IRpcMessageHandler {
public:
    void handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) override;
};
