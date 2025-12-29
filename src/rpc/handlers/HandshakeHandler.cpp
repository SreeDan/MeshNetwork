#include "HandshakeHandler.h"

#include "EnvelopeUtils.h"
#include "IRpcMessageHandler.h"
#include "RpcConnection.h"

void HandshakeHandler::handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) {
    if (!env.expect_response()) return;

    // Deserialize the request
    mesh::Handshake req;
    if (!req.ParseFromString(env.payload())) {
        std::cerr << "Failed to parse handshake request\n";
        return;
    }

    // Not the initiator
    if (env.expect_response()) {
        auto response = mesh::envelope::MakeHandshakeResponse(
            conn->get_local_peer_ip(),
            env.from(),
            conn->get_self_peer_id()
        );
        response.set_msg_id(env.msg_id());
        conn->send_message(response);
    }

    conn->fulfill_handshake_promise(req.peer_record());
}
