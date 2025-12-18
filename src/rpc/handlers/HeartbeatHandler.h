#pragma once
#include "IMessageHandler.h"

class RpcConnection;

class HeartbeatHandler : public IMessageHandler {
public:
    void handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) override;
};
