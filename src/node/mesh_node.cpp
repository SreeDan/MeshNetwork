#include "mesh_node.h"

#include <filesystem>
#include <iostream>
#include <utility>

#include "EnvelopeUtils.h"
#include "GraphManager.h"
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
            std::cout << "on top resp" << std::endl;
            auto neighbors = router_->get_direct_neighbors();
            mesh::TopologyResponse resp;
            for (auto &neighbor_peer_id: neighbors) {
                resp.add_directly_connected_peers(neighbor_peer_id);
                std::cout << "neighbor peer: " << neighbor_peer_id << std::endl;
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
            std::cout << "ping from " << from << std::endl;
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
                std::cout << "Incoming connection from " << remote_addr << ":" <<
                        remote_port << std::endl;
                rpc_connections->accept_connection(remote_addr, std::move(sock));
            }
        } else {
            std::cerr << "Accept failed: " << ec.message() << std::endl;
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
            std::cout << "connected to remote peer_id \"" << peer_response.value() << "\"" << std::endl;
        } else {
            std::cerr << "connect_to failed to connect to remote" << peer_response.error() << std::endl;
        }
    } catch (std::exception &e) {
        std::cerr << "connect_to failed: " << e.what() << std::endl;
    }
}

void MeshNode::send_message(const std::string &remote_id, const std::string &text) {
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
    std::cout << "registering subtype " << subtype << std::endl;

    handlers_[subtype] = [handler](const std::string &from, const std::string &raw_bytes, auto reply_cb) {
        if (T msg; msg.ParseFromString(raw_bytes)) {
            handler(from, msg, std::move(reply_cb));
        } else {
            std::cerr << "Failed to parse " << T::descriptor()->full_name() << std::endl;
        }
    };
}

void MeshNode::ping(const std::string &peer) {
    mesh::PingRequest req;
    auto future = send_request<mesh::PingResponse>(peer, req);

    std::pair<std::string, mesh::PingResponse> resp;
    try {
        resp = future.get();
        std::cout << "pong from " << resp.first << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "Ping request failed: " << e.what() << std::endl;
    }
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
            std::cerr << "[MeshNode] No handler for subtype: " << pkt.subtype() << std::endl;
        }
    }
    // Case 2: legacy/debug text messages
    else if (pkt.type() == mesh::PacketType::TEXT) {
        std::cout << "[MeshNode] Text from " << from << ": " << pkt.text() << std::endl;
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
        std::cerr << "Topology request failed: " << e.what() << std::endl;
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
