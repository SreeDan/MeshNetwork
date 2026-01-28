#include "HandshakeHandler.h"

#include "EnvelopeUtils.h"
#include "IRpcMessageHandler.h"
#include "Logger.h"
#include "RpcConnection.h"

void HandshakeHandler::handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) {
    if (!env.expect_response()) return;

    // Deserialize the request
    mesh::Handshake req;
    if (!req.ParseFromString(env.payload())) {
        Log::warn("handshake_handler",
                  {{"peer_id", conn.get()->get_remote_peer_id()}},
                  "failed to parse handshake request"
        );
        return;
    }

    auto remote_address = conn->remote_endpoint_.address();
    conn->remote_udp_endpoint_ = boost::asio::ip::udp::endpoint(remote_address, req.peer_record().peer_ip().udp_port());

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
