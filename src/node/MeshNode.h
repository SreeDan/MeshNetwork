#pragma once
#include <string>
#include <boost/asio.hpp>

#include "AsyncRequestTracker.h"
#include "Logger.h"
#include "MeshRouter.h"
#include "RoutedPacketUtils.h"
#include "RpcManager.h"
#include "StringUtils.h"
#include "topology.pb.h"


class MeshNode {
public:
    MeshNode(
        boost::asio::io_context &ioc,
        int tcp_port,
        int udp_port,
        const std::string &peer_id,
        std::shared_ptr<boost::asio::ssl::context> ssl_ctx);

    void start();

    void stop();

    void connect(const std::string &host, int port);

    void set_output_directory(const std::string &dir);

    void generate_topology_graph(const std::string &filename);

    void send_text(const std::string &peer_id, const std::string &text);

    template<typename T>
        requires std::derived_from<T, google::protobuf::Message>
    void send_message(const std::string &dest_id, const T &msg);

    template<typename T>
        requires std::derived_from<T, google::protobuf::Message>
    void broadcast_message(const T &msg);

    template<typename ResponseT, typename RequestT>
        requires std::derived_from<ResponseT, google::protobuf::Message> &&
                 std::derived_from<RequestT, google::protobuf::Message>
    std::future<std::pair<std::string, ResponseT> > send_request(
        const std::string &dest_id, const RequestT &req,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );


    template<typename ResponseT, typename RequestT>
    std::future<std::vector<std::pair<std::string, ResponseT> > > broadcast_request(
        const RequestT &req,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );

    // Register a handler for a specific message type.
    // The handler receives:
    //  - from: the ID of the sender
    //  - msg: the parsed proto object
    //  - reply: a function to send a response back (optional)
    template<typename T>
        requires std::derived_from<T, google::protobuf::Message>
    void on(std::function<void(const std::string &from, const T &msg,
                               std::function<void(std::string &)> reply)> handler);

    bool ping(const std::string &peer);

    std::vector<std::string> get_nodes_in_network();

    void set_block_all_messages(bool block);

    void add_auto_connection(const std::string &ip_address, int port);

    void remove_auto_connection(const std::string &ip_address, int port);

private:
    boost::asio::io_context &ioc_;
    int tcp_port_;
    int udp_port_;
    std::string peer_id_;
    std::shared_ptr<boost::asio::ssl::context> ssl_ctx_;
    std::string output_directory_;
    bool ignore_all_incoming_messages_ = false;

    // Stack components
    std::shared_ptr<RpcManager> rpc_connections; // Transport
    std::shared_ptr<MeshRouter> router_; // Routing
    // std::shared_ptr<IdentityManager> identity_; // Peer identity and certificate management
    // std::unique_ptr<PacketSecurity> security_; // Packet encryption
    AsyncRequestTracker<std::string> request_tracker_; // Manage request lifecycle

    using PacketHandler = std::function<void(const std::string &from,
                                             const std::string &raw_bytes,
                                             std::function<void(std::string &)> reply_cb
        )
    >;
    std::unordered_map<std::string, PacketHandler> handlers_;

    void handle_incoming_packet(const mesh::RoutedPacket &pkt);

    void setup_builtin_handlers();

    // void dispatch_message(const std::string &from, const mesh::RoutedPacket &pkt);
    //
    // void do_accept();
};


template<typename T>
    requires std::derived_from<T, google::protobuf::Message>
void MeshNode::send_message(const std::string &dest_id, const T &msg) {
    auto pkt = mesh::packet::MakeBinaryRoutedPacket(
        peer_id_, dest_id, MeshRouter::DEFAULT_TTL,
        T::descriptor()->full_name(),
        msg.SerializeAsString(),
        false
    );
    router_->send_packet(pkt);
}

template<typename T>
    requires std::derived_from<T, google::protobuf::Message>
void MeshNode::broadcast_message(const T &msg) {
    auto pkt = mesh::packet::MakeBinaryRoutedPacket(
        peer_id_, "", MeshRouter::DEFAULT_TTL,
        T::descriptor()->full_name(),
        msg.SerializeAsString(),
        false
    );
    router_->send_packet(pkt);
}

template<typename ResponseT, typename RequestT>
    requires std::derived_from<ResponseT, google::protobuf::Message> &&
             std::derived_from<RequestT, google::protobuf::Message>
std::future<std::pair<std::string, ResponseT> > MeshNode::send_request(
    const std::string &dest_id, const RequestT &req,
    std::chrono::milliseconds timeout) {
    auto pkt = mesh::packet::MakeBinaryRoutedPacket(
        peer_id_, dest_id, MeshRouter::DEFAULT_TTL,
        RequestT::descriptor()->full_name(),
        req.SerializeAsString(),
        true
    );

    std::string req_id = to_hex(pkt.id());
    auto future = request_tracker_.track_request(req_id, std::chrono::seconds(5));

    router_->send_packet(pkt);

    return std::async(std::launch::deferred, [dest_id, raw_fut = std::move(future)]() mutable {
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

    const std::string &req_id = to_hex(pkt.id());
    request_tracker_.track_broadcast(req_id, timeout, on_response, on_complete);

    router_->send_packet(pkt);

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

