#include "EnvelopeUtils.h"

#include "envelope.pb.h"
#include "MessageUtils.h"

namespace mesh {
    namespace envelope {
        PeerIP MakePeerIP(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint) {
            PeerIP ip;
            ip.set_ip(endpoint.address().to_string());
            ip.set_port(endpoint.port());
            return ip;
        }


        PeerIP MakePeerIP(const std::string &addr, int port) {
            PeerIP ip;
            ip.set_ip(addr);
            ip.set_port(port);
            return ip;
        }

        Envelope MakeCustomText(const PeerIP &from,
                                const PeerIP &to,
                                const std::string &payload) {
            Envelope e;
            *e.mutable_from() = from;
            *e.mutable_to() = to;
            e.set_msg_id(generate_uuid_bytes());
            e.set_payload(payload);
            e.set_type(mesh::DATA);
            e.set_expect_response(false);
            return e;
        }

        Envelope MakeHandshakeRequest(const PeerIP &from,
                                      const PeerIP &to,
                                      const std::string &peer_id) {
            PeerRecord r;
            r.set_peer_id(peer_id);
            *r.mutable_peer_ip() = from;
            Handshake h;
            h.set_proto_version(MeshVersion);
            *h.mutable_peer_record() = r;

            Envelope e;
            e.set_msg_id(generate_uuid_bytes());
            *e.mutable_from() = from;
            *e.mutable_to() = to;

            *h.mutable_peer_record() = r;
            e.set_type(mesh::HANDSHAKE);
            e.set_payload(h.SerializeAsString());
            e.set_expect_response(true);
            return e;
        }


        Envelope MakeHandshakeResponse(const PeerIP &from,
                                       const PeerIP &to,
                                       const std::string &peer_id) {
            PeerRecord r;
            r.set_peer_id(peer_id);
            *r.mutable_peer_ip() = from;
            Handshake h;
            h.set_proto_version(MeshVersion);
            *h.mutable_peer_record() = r;

            Envelope e;
            *e.mutable_from() = from;
            *e.mutable_to() = to;
            e.set_msg_id(generate_uuid_bytes());

            e.set_type(mesh::HANDSHAKE);
            e.set_payload(h.SerializeAsString());
            e.set_expect_response(false);
            return e;
        }

        Envelope MakeHeartbeatRequest(const PeerIP &from,
                                      const PeerIP &to) {
            Envelope e;
            e.set_msg_id(generate_uuid_bytes());
            *e.mutable_from() = from;
            *e.mutable_to() = to;

            *e.mutable_payload() = "";
            e.set_type(mesh::HEARTBEAT);
            e.set_expect_response(true);
            return e;
        }

        Envelope MakeHeartbeatResponse(const PeerIP &from,
                                       const PeerIP &to) {
            Envelope e;
            e.set_msg_id(generate_uuid_bytes());
            *e.mutable_from() = from;
            *e.mutable_to() = to;

            *e.mutable_payload() = "";
            e.set_type(mesh::HEARTBEAT);
            e.set_expect_response(false);
            return e;
        }

        Envelope MakeGenericData(const std::string &payload) {
            Envelope e;
            e.set_msg_id(generate_uuid_bytes());
            *e.mutable_payload() = payload;
            e.set_type(mesh::DATA);
            return e;
        }
    }
}
