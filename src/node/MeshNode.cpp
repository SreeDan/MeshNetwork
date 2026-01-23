#include "MeshNode.h"

#include <filesystem>
#include <iostream>
#include <utility>

#include "EnvelopeUtils.h"
#include "GraphManager.h"
#include "Logger.h"
#include "ping.pb.h"

MeshNode::MeshNode(
    boost::asio::io_context &ioc,
    const int tcp_port,
    const int udp_port,
    const std::string &peer_id,
    std::shared_ptr<boost::asio::ssl::context> ssl_ctx)
    : ioc_(ioc),
      tcp_port_(tcp_port),
      udp_port_(udp_port),
      peer_id_(peer_id),
      ssl_ctx_(std::move(ssl_ctx)),
      rpc_connections(std::make_shared<RpcManager>(ioc, peer_id, tcp_port)),
      router_(std::make_shared<MeshRouter>(ioc, peer_id)),
      request_tracker_(ioc) {
    rpc_connections->set_sink(router_);
    router_->set_transport(rpc_connections);
    router_->set_on_packet_for_me(
        [this](const mesh::RoutedPacket &pkt) {
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
    on<mesh::TopologyRequest>(
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

    on<mesh::PingRequest>(
        [this](const std::string &from, const mesh::PingRequest &req, auto reply) {
            mesh::PingResponse resp;
            std::string bytes = resp.SerializeAsString();
            Log::info("ping", {{"peer_id", from}}, "ping received");
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

    auto future = send_request<mesh::PingResponse>(peer, req);

    std::pair<std::string, mesh::PingResponse> resp;
    try {
        resp = future.get();
        Log::info("ping", {{"peer_id", resp.first}}, "pong received");
        return true;
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

void MeshNode::handle_incoming_packet(const mesh::RoutedPacket &pkt) {
    if (ignore_all_incoming_messages_) return;

    // TODO: handle decryption

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
            auto reply_cb = [this, from, req_id = pkt.id(), needs_reply = pkt.expect_response()]
            (std::string &response_payload) {
                if (!needs_reply) return;

                // Create the Response Packet matching the original ID
                auto resp = mesh::packet::MakeBinaryRoutedPacket(
                    peer_id_, from, 15, "",
                    response_payload,
                    false
                );
                resp.set_id(req_id);

                // TODO: handle encryption

                router_->send_packet(resp);
            };


            it->second(from, pkt.binary_data(), reply_cb);
        } else {
            Log::warn("MeshNode", {{"subtype", pkt.subtype()}}, "No handler for message type");
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

    auto future = broadcast_request<mesh::TopologyResponse>(req);

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


void MeshNode::add_auto_connection(const std::string &ip_address, int port) {
    auto peer_ip = mesh::envelope::MakePeerIP(ip_address, port);
    rpc_connections->add_auto_connection(peer_ip);
}

void MeshNode::remove_auto_connection(const std::string &ip_address, int port) {
    auto peer_ip = mesh::envelope::MakePeerIP(ip_address, port);
    rpc_connections->remove_auto_connection(peer_ip);
}
