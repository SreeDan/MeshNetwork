#pragma once
#include "IRpcMessageHandler.h"

class TextHandler : public IRpcMessageHandler {
public:
    void handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) override;
};

