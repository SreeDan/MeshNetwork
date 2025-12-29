#include "EnvelopeUtils.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "envelope.pb.h"

namespace mesh {
    boost::uuids::uuid gen_uuid() {
        static boost::uuids::random_generator generator;
        return generator();
    }

    namespace envelope {
        PeerIP MakePeerIP(boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> endpoint) {
            PeerIP ip;
            ip.set_ip(endpoint.address().to_string());
            ip.set_port(endpoint.port());
            return ip;
        }

        Envelope MakeCustomText(const PeerIP &from,
                                const PeerIP &to,
                                const std::string &payload) {
            Envelope e;
            *e.mutable_from() = from;
            *e.mutable_to() = to;
            e.set_msg_id(gen_uuid().data, 16);
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
            e.set_msg_id(gen_uuid().data, 16);
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
            e.set_msg_id(gen_uuid().data, 16);

            e.set_type(mesh::HANDSHAKE);
            e.set_payload(h.SerializeAsString());
            e.set_expect_response(false);
            return e;
        }

        Envelope MakeHeartbeatRequest(const PeerIP &from,
                                      const PeerIP &to) {
            Envelope e;
            e.set_msg_id(gen_uuid().data, 16);
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
            e.set_msg_id(gen_uuid().data, 16);
            *e.mutable_from() = from;
            *e.mutable_to() = to;

            *e.mutable_payload() = "";
            e.set_type(mesh::HEARTBEAT);
            e.set_expect_response(false);
            return e;
        }
    }
}
