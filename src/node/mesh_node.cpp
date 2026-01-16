#include "mesh_node.h"

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
      acceptor_(ioc),
      tcp_port_(tcp_port),
      udp_port_(udp_port),
      peer_id_(peer_id),
      ssl_ctx_(std::move(ssl_ctx)),
      rpc_connections(std::make_shared<RpcManager>(ioc, peer_id)),
      router_(std::make_shared<MeshRouter>(ioc, peer_id)) {
    boost::system::error_code ec;

    acceptor_.open(boost::asio::ip::tcp::v4(), ec);
    if (ec) throw std::runtime_error("open failed: " + ec.message());

    // Helps when restarting after crashes / TIME_WAIT
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);

    acceptor_.bind(
        boost::asio::ip::tcp::endpoint(
            boost::asio::ip::tcp::v4(), tcp_port_),
        ec);

    if (ec)
        throw std::runtime_error(
            "bind failed on port " + std::to_string(tcp_port_) +
            ": " + ec.message());

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) throw std::runtime_error("listen failed: " + ec.message());

    rpc_connections->set_sink(router_);
    router_->set_transport(rpc_connections);
    router_->set_on_message_received(
        [this](const std::string &from_id, const mesh::RoutedPacket &pkt) {
            this->handle_received_message(from_id, pkt);
        });
}

void MeshNode::start() {
    enable_topology_features();
    enable_ping_features();
    do_accept();
}

void MeshNode::stop() {
    boost::system::error_code ec;
    acceptor_.close(ec);
}

void MeshNode::enable_topology_features() {
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
}

void MeshNode::enable_ping_features() {
    on<mesh::PingRequest>(
        [this](const std::string &from, const mesh::PingRequest &req, auto reply) {
            mesh::PingResponse resp;
            std::string bytes = resp.SerializeAsString();
            Log::info("ping", {{"peer_id", from}}, "ping received");
            reply(bytes);
        }
    );
}


void MeshNode::do_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket sock) {
        if (ec == boost::asio::error::operation_aborted) return;

        if (!ec) {
            boost::system::error_code ec2;
            auto remote_ep = sock.remote_endpoint(ec2);
            if (!ec2) {
                auto remote_addr = remote_ep.address().to_string();
                auto remote_port = remote_ep.port();
                Log::info("mesh_node.acceptor",
                          {{"from", remote_addr}, {"port", remote_port}},
                          "incoming connection");
                rpc_connections->accept_connection(remote_addr, std::move(sock));
            }
        } else {
            Log::error("acceptor", {{"err", ec.message()}}, "accept failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        do_accept();
    });
}

void MeshNode::connect_to(const std::string &host, int port) {
    try {
        boost::asio::ip::tcp::resolver resolver(ioc_);
        auto endpoints = resolver.resolve(host, std::to_string(port));
        boost::asio::ip::tcp::socket sock(ioc_);
        boost::asio::connect(sock, endpoints);
        auto remote_addr = sock.remote_endpoint().address().to_string();
        std::expected<std::string, std::string> peer_response = rpc_connections->create_connection(
            remote_addr, std::move(sock));

        if (peer_response.has_value()) {
            Log::info("mesh_node.connect",
                      {{"peer_id", peer_response.value()}},
                      "connected to remote peer");
        } else {
            Log::error("mesh_node.connect",
                       {{"err", peer_response.error()}},
                       "failed to connect to remote peer");
        }
    } catch (std::exception &e) {
        Log::error("mesh_node.connect",
                   {"err", e.what()},
                   "failed to connect to remote");
    }
}

void MeshNode::send_text(const std::string &remote_id, const std::string &text) {
    router_->send_text(remote_id, text);
}


template<typename T>
void MeshNode::send(const std::string &dest_id, const T &msg) {
    auto pkt = mesh::packet::MakeBinaryRoutedPacket(
        peer_id_, dest_id, DEFAULT_TTL,
        T::descriptor()->full_name(),
        msg.SerializeAsString(),
        false
    );
    router_->send_packet(pkt);
}

template<typename ResponseT, typename RequestT>
std::future<std::pair<std::string, ResponseT> > MeshNode::send_request(
    const std::string &dest_id, const RequestT &req,
    std::chrono::milliseconds timeout) {
    auto pkt = mesh::packet::MakeBinaryRoutedPacket(
        peer_id_, dest_id, 15,
        RequestT::descriptor()->full_name(),
        req.SerializeAsString(),
        true
    );

    std::future<std::string> raw_fut = router_->send_request(pkt, timeout);

    return std::async(std::launch::deferred, [dest_id, raw_fut = std::move(raw_fut)]() mutable {
        std::string bytes = raw_fut.get();
        ResponseT resp;
        if (!resp.ParseFromString(bytes)) {
            throw std::runtime_error("Failed to parse response type");
        }

        return std::make_pair(dest_id, resp);
    });
}


template<typename ResponseT, typename RequestT>
std::future<std::vector<std::pair<std::string, ResponseT> > > MeshNode::broadcast_request(
    const RequestT &req,
    std::chrono::milliseconds timeout) {
    // Shared state to collect responses
    auto results = std::make_shared<std::vector<std::pair<std::string, ResponseT> > >();
    auto prom = std::make_shared<std::promise<std::vector<std::pair<std::string, ResponseT> > > >();
    auto pkt = mesh::packet::MakeBinaryRoutedPacket(
        peer_id_, "", 15, // Empty dest_id = Broadcast
        RequestT::descriptor()->full_name(),
        req.SerializeAsString(),
        true
    );

    // Lambda called on EVERY response
    auto on_response = [results, this](const std::string &sender_peer_id, std::string raw_bytes) {
        ResponseT resp;
        if (resp.ParseFromString(raw_bytes)) {
            results->push_back({sender_peer_id, resp});
        }
    };

    // Lambda called when timeout finishes
    auto on_complete = [results, prom]() {
        prom->set_value(*results);
    };

    router_->send_broadcast_request(pkt, timeout, on_response, on_complete);

    return prom->get_future();
}

template<typename T>
    requires std::derived_from<T, google::protobuf::Message>
void MeshNode::on(std::function<void(const std::string &from, const T &msg,
                                     std::function<void(std::string &)> reply)> handler) {
    std::string subtype = T::descriptor()->full_name();
    Log::debug("mesh_node.on", {"subtype", subtype}, "registering subtype");

    handlers_[subtype] = [handler](const std::string &from, const std::string &raw_bytes, auto reply_cb) {
        if (T msg; msg.ParseFromString(raw_bytes)) {
            handler(from, msg, std::move(reply_cb));
        } else {
            Log::error("mesh_node.on", {"subtype", T::descriptor()->full_name()},
                       "failed to parse protobuf subtype message");
        }
    };
}

void MeshNode::ping(const std::string &peer) {
    mesh::PingRequest req;

    auto future = send_request<mesh::PingResponse>(peer, req);

    std::pair<std::string, mesh::PingResponse> resp;
    try {
        resp = future.get();
        Log::info("ping", {{"peer_id", resp.first}}, "pong received");
    } catch (const std::exception &e) {
        Log::error("ping", {{"err", e.what()}}, "ping request failed");
    }
}

std::vector<std::string> MeshNode::get_nodes_in_network() {
    return router_->get_peers_in_network();
}

void MeshNode::set_block_all_messages(bool block) {
    router_->set_ignore_messages(block);
}

void MeshNode::handle_received_message(const std::string &from, const mesh::RoutedPacket &pkt) {
    if (pkt.type() == mesh::PacketType::BINARY) {
        auto it = handlers_.find(pkt.subtype());
        if (it != handlers_.end()) {
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

                router_->send_packet(resp);
            };

            // second is the handler
            it->second(from, pkt.binary_data(), reply_cb);
        } else {
            Log::warn("received_message",
                      {{"subtype", pkt.subtype()}},
                      "no handler for subtype");
        }
    }
    // Case 2: legacy/debug text messages
    else if (pkt.type() == mesh::PacketType::TEXT) {
        Log::info("received_message",
                  {{"message", pkt.text()}, {"from", from}},
                  "text received");
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
