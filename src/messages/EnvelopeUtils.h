#pragma once

#include <boost/asio/ip/basic_endpoint.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "messages.pb.h"

namespace mesh {
    const std::string MeshVersion = "1.0.0";

    namespace envelope {
        PeerIP MakePeerIP(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint);

        Envelope MakeCustomText(
            const PeerIP &from,
            const PeerIP &to,
            const std::string &payload
        );

        Envelope MakeHandshakeRequest(
            const PeerIP &from,
            const PeerIP &to,
            const std::string &peer_id
        );

        Envelope MakeHandshakeResponse(
            const PeerIP &from,
            const PeerIP &to,
            const std::string &peer_id
        );
    }
}

