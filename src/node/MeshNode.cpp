#include "MeshNode.h"

#include <filesystem>
#include <iostream>
#include <utility>

#include "CertHelpers.h"
#include "crypto.pb.h"
#include "EnvelopeUtils.h"
#include "GraphManager.h"
#include "Logger.h"
#include "ping.pb.h"

MeshNode::MeshNode(
    boost::asio::io_context &ioc,
    const int tcp_port,
    const int udp_port,
    const std::string &peer_id,
    std::shared_ptr<boost::asio::ssl::context> ssl_ctx,
    std::shared_ptr<IdentityManager> identity_manager,
    bool encrypt_messages)
    : ioc_(ioc),
      tcp_port_(tcp_port),
      udp_port_(udp_port),
      peer_id_(peer_id),
      ssl_ctx_(std::move(ssl_ctx)),
      rpc_connections(std::make_shared<RpcManager>(ioc, peer_id, tcp_port, udp_port)),
      router_(std::make_shared<MeshRouter>(ioc, peer_id)),
      identity_(identity_manager),
      security_(std::make_unique<PacketSecurity>(identity_manager)),
      request_tracker_(ioc),
      encrypt_messages_(encrypt_messages),
      identity_request_subtype(mesh::IdentityRequest::descriptor()->full_name()),
      identity_response_subtype(mesh::IdentityResponse::descriptor()->full_name()) {
    rpc_connections->set_sink(router_);
    router_->set_transport(rpc_connections);
    router_->set_on_packet_for_me(
        [this](mesh::RoutedPacket &pkt) {
            this->handle_incoming_packet(pkt);
        });
}

void MeshNode::start() {
    setup_builtin_handlers();
    router_->start();
}

void MeshNode::stop() {
    rpc_connections->shutdown();
}

void MeshNode::setup_builtin_handlers() {
    on<mesh::TopologyRequest, mesh::TopologyResponse>(
        [this](const std::string &from, const mesh::TopologyRequest &req, auto reply) {
            auto neighbors = router_->get_direct_neighbors();
            mesh::TopologyResponse resp;
            for (auto &neighbor_peer_id: neighbors) {
                resp.add_directly_connected_peers(neighbor_peer_id);
            }

            std::string bytes = resp.SerializeAsString();
            reply(bytes);
        }
    );

    on<mesh::PingRequest, mesh::PingResponse>(
        [this](const std::string &from, const mesh::PingRequest &req, auto reply) {
            mesh::PingResponse resp;
            resp.set_message("hello from" + peer_id_);
            std::string bytes = resp.SerializeAsString();
            Log::info("ping", {{"peer_id", from}, {"message", req.message()}}, "ping received");
            reply(bytes);
        }
    );

    on<mesh::IdentityRequest, mesh::IdentityResponse>(
        [this](const std::string &from, const mesh::IdentityRequest &req, auto reply) {
            try {
                std::string pub_key = CertHelpers::extract_pubkey_from_cert(req.certificate_pem());
                this->identity_->add_trusted_peer(from, pub_key);
                handle_pending_incoming_packets(from);
                handle_pending_outgoing_packets(from);
            } catch (const std::exception &e) {
                Log::error("identity_request", {{"peer", from}, {"err", e.what()}},
                           "failed to extract public key from response");
            }

            mesh::IdentityResponse resp;
            resp.set_certificate_pem(identity_->get_my_cert());
            std::string bytes = resp.SerializeAsString();
            reply(bytes);
        }
    );
}


void MeshNode::connect(const std::string &host, int port) {
    try {
        std::expected<std::string, std::string> peer_response = rpc_connections->connect(host, port);

        if (peer_response.has_value()) {
            Log::info("connect_to",
                      {{"peer_id", peer_response.value()}},
                      "connected to remote peer");
        } else {
            Log::error("connect_to",
                       {{"err", peer_response.error()}},
                       "failed to connect to remote peer");
            throw std::runtime_error("failed to connect to remote peer");
        }
    } catch (std::exception &e) {
        Log::error("mesh_node.connect",
                   {"err", e.what()},
                   "failed to connect to remote");
    }
}

void MeshNode::send_text(const std::string &remote_id, const std::string &text) {
    mesh::RoutedPacket pkt = mesh::packet::MakeTextRoutedPacket(peer_id_, remote_id, MeshRouter::DEFAULT_TTL, text);
    router_->send_packet(pkt);
}


bool MeshNode::ping(const std::string &peer) {
    mesh::PingRequest req;
    req.set_message("hello from " + peer_id_);

    auto future = send_request<mesh::PingRequest, mesh::PingResponse>(peer, req);

    std::expected<std::pair<std::string, mesh::PingResponse>, RequestError> resp;
    try {
        resp = future.get();
        if (resp.has_value()) {
            Log::info("ping", {{"peer_id", resp.value().first}, {"message", resp.value().second.message()}},
                      "pong received");
            return true;
        }

        Log::error("ping", {{"err", resp.error()}}, "ping request failed");
        return false;
    } catch (const std::exception &e) {
        Log::error("ping", {{"err", e.what()}}, "ping request failed");
        return false;
    }
}

std::vector<std::string> MeshNode::get_nodes_in_network() {
    return router_->get_peers_in_network();
}

void MeshNode::set_block_all_messages(bool block) {
    ignore_all_incoming_messages_ = block;
}

bool MeshNode::should_send_identity_request(const std::string &peer_id) {
    auto now = std::chrono::steady_clock::now();

    auto it = id_request_cooldowns_.find(peer_id);
    if (it != id_request_cooldowns_.end()) {
        auto last_request_time = it->second;
        auto time_since = std::chrono::duration_cast<std::chrono::seconds>(now - last_request_time);

        // If we asked less than 5 seconds ago, block the request
        if (time_since.count() < 5) {
            return false;
        }
    }

    id_request_cooldowns_[peer_id] = now;
    return true;
}

void MeshNode::handle_incoming_packet(mesh::RoutedPacket &pkt) {
    if (ignore_all_incoming_messages_) return;

    if (pkt.has_encrypted_payload()) {
        const std::string &from = pkt.from_peer_id();
        if (!identity_->has_key(from)) {
            Log::debug("handle_incoming_packet",
                       {{"from", from}},
                       "buffering packet, don't have public key to decrypt");
            identity_->buffer_incoming_packet(from, pkt);

            if (should_send_identity_request(from)) {
                Log::info("MeshNode", {{"target", from}}, "Unknown Identity. Sending Request.");
                send_identity_request(from);
            }
            return;
        }

        auto expected_plaintext = security_->decrypt_packet(pkt);
        if (!expected_plaintext.has_value()) {
            Log::warn("handle_incoming_packet",
                      {{"from", pkt.from_peer_id()}, {"err", expected_plaintext.error()}},
                      "failed to decrypt packet");
            return;
        }

        pkt.clear_encrypted_payload();
        pkt.clear_signature();
        pkt.clear_encrypted_session_key();
        pkt.set_binary_data(expected_plaintext.value());
    }

    std::string pkt_id = to_hex(pkt.id());
    // If this was a reply. We should set the future and not send another request.
    if (request_tracker_.fulfill_request(pkt_id, pkt.from_peer_id(), pkt.binary_data())) {
        return;
    }

    // It's a new message, and check if we have a protobuf handler for it
    if (pkt.type() == mesh::PacketType::BINARY) {
        auto it = handlers_.find(pkt.subtype());
        if (it != handlers_.end()) {
            const std::string &from = pkt.from_peer_id();
            auto reply_cb = [this, from, pkt = pkt, subtype = it->second.ResponseSubtype]
            (std::string &response_payload) {
                if (!pkt.expect_response()) return;

                // Create the Response Packet matching the original ID
                auto resp = mesh::packet::MakeBinaryRoutedPacket(
                    peer_id_, from, 15, subtype, pkt.transport(),
                    response_payload,
                    false
                );
                resp.set_id(pkt.id());

                bool is_identity_exchange = (pkt.subtype() == this->identity_request_subtype);

                if (this->encrypt_messages_ && !is_identity_exchange) {
                    if (!this->security_->secure_packet(resp, from, response_payload)) {
                        Log::error("handle_incoming_packet", {{"to", from}}, "failed to encrypt response");
                        return;
                    }
                } else {
                    resp.set_binary_data(response_payload);
                }

                router_->send_packet(resp);
            };

            it->second.handler(from, pkt.binary_data(), reply_cb);
        } else {
            Log::debug("MeshNode", {{"subtype", pkt.subtype()}}, "No handler for message type");
        }
    } else if (pkt.type() == mesh::PacketType::TEXT) {
        Log::info("message", {{"from", pkt.from_peer_id()}}, pkt.text());
    }
}

void MeshNode::set_output_directory(const std::string &dir) {
    output_directory_ = dir;
}

void MeshNode::generate_topology_graph(const std::string &filename) {
    mesh::TopologyRequest req;

    auto future = broadcast_request<mesh::TopologyRequest, mesh::TopologyResponse>(req);

    std::vector<std::pair<std::string, mesh::TopologyResponse> > broadcast_responses;
    try {
        broadcast_responses = future.get();
    } catch (const std::exception &e) {
        Log::error("topology", {{"err", e.what()}}, "topology request failed");
        return;
    }
    std::filesystem::path dir{output_directory_};
    std::filesystem::path dest = dir / filename;

    UndirectedGraphManager graph;
    for (const auto &[sender_peer_id, resp]: broadcast_responses) {
        for (const auto &direct_neighbor: resp.directly_connected_peers()) {
            graph.add_connection(sender_peer_id, direct_neighbor);
        }
    }

    graph.save_graph(dest);
}


void MeshNode::add_auto_connection(const std::string &ip_address, int tcp_port) {
    auto peer_ip = mesh::envelope::MakePeerIP(ip_address, tcp_port, 0);
    rpc_connections->add_auto_connection(peer_ip);
}

void MeshNode::remove_auto_connection(const std::string &ip_address, int tcp_port) {
    auto peer_ip = mesh::envelope::MakePeerIP(ip_address, tcp_port, 0);
    rpc_connections->remove_auto_connection(peer_ip);
}


void MeshNode::send_identity_request(const std::string &dest) {
    mesh::IdentityRequest req;
    req.set_certificate_pem(identity_->get_my_cert());
    send_request_async<mesh::IdentityRequest, mesh::IdentityResponse>(
        dest,
        req,
        [this, dest](const std::string &peer, std::expected<mesh::IdentityResponse, RequestError> resp) {
            if (!resp || !resp.has_value()) {
                Log::warn("identity_request", {{"from", peer}}, "identity request timed out");
                return;
            }

            const auto &unwrapped_resp = resp.value();

            try {
                std::string pub_key = CertHelpers::extract_pubkey_from_cert(unwrapped_resp.certificate_pem());
                this->identity_->add_trusted_peer(peer, pub_key);
                handle_pending_incoming_packets(peer);
                handle_pending_outgoing_packets(peer);
            } catch (const std::exception &e) {
                Log::error("identity_request", {{"peer", peer}, {"err", e.what()}},
                           "failed to extract public key from response");
            }
        }
    );
}

void MeshNode::handle_pending_incoming_packets(const std::string &src) {
    std::vector<mesh::RoutedPacket> packets = identity_->pop_incoming_pending_packets(src);
    for (auto &pkt: packets) {
        handle_incoming_packet(pkt);
    }
}

void MeshNode::handle_pending_outgoing_packets(const std::string &dest) {
    std::vector<mesh::RoutedPacket> packets = identity_->pop_outgoing_pending_packets(dest);
    for (auto &pkt: packets) {
        if (!security_->secure_packet(pkt, pkt.to_peer_id(), pkt.binary_data())) {
            continue;
        }
        router_->send_packet(pkt);
    }
}
