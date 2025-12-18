#include "HeartbeatHandler.h"

#include "EnvelopeUtils.h"
#include "RpcConnection.h"


void HeartbeatHandler::handle(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &env) {
    if (!env.expect_response()) return;
    std::cout << "in heartbeat handler" << std::endl;

    auto response = mesh::envelope::MakeHeartbeatResponse(
        conn->get_local_peer_ip(),
        env.from()
    );

    response.set_msg_id(env.msg_id());
    conn->send_message(response);
}
