#pragma once
#include "IMessageHandler.h"

class TextHandler : public IMessageHandler {
public:
    void handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) override;
};

