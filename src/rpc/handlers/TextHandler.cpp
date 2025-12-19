#include "TextHandler.h"

void TextHandler::handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) {
    std::cout << "Received a message with payload: \n" << env.payload() << std::endl;
}
