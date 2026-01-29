#pragma once
#include <string>
#include <boost/asio.hpp>
#include <vector>
#include <future>
#include <chrono>
#include <expected>

#include "AsyncRequestTracker.h"
#include "Logger.h"
#include "MeshRouter.h"
#include "PacketSecurity.h"
#include "RoutedPacketUtils.h"
#include "RpcManager.h"
#include "StringUtils.h"
#include "topology.pb.h"


enum class RequestError {
    RequestTimeout,
    EncryptionError,
    MalformedResponse,
};

class MeshNode {
public:
    MeshNode(
        boost::asio::io_context &ioc,
        int tcp_port,
        int udp_port,
        const std::string &peer_id,
        std::shared_ptr<boost::asio::ssl::context> ssl_ctx,
        std::shared_ptr<IdentityManager> identity_manager_,
        bool encrypt_messages);

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

    template<typename RequestT, typename ResponseT>
        requires std::derived_from<ResponseT, google::protobuf::Message> &&
                 std::derived_from<RequestT, google::protobuf::Message>
    void send_request_async(
        const std::string &dest_id,
        const RequestT &req,
        std::function<void(const std::string &, std::expected<ResponseT, RequestError>)> callback,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );

    template<typename RequestT, typename ResponseT>
        requires std::derived_from<ResponseT, google::protobuf::Message> &&
                 std::derived_from<RequestT, google::protobuf::Message>
    std::future<std::expected<std::pair<std::string, ResponseT>, RequestError> > send_request(
        const std::string &dest_id, const RequestT &req,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)
    );


    template<typename RequestT, typename ResponseT>
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
    bool encrypt_messages_;
    std::string identity_request_subtype;
    std::string identity_response_subtype;
    std::string output_directory_;
    bool ignore_all_incoming_messages_ = false;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> id_request_cooldowns_;

    // Stack components
    std::shared_ptr<RpcManager> rpc_connections; // Transport
    std::shared_ptr<MeshRouter> router_; // Routing
    std::shared_ptr<IdentityManager> identity_; // Peer identity and certificate management
    std::unique_ptr<PacketSecurity> security_; // Packet encryption
    AsyncRequestTracker<std::string> request_tracker_; // Manage request lifecycle


    using PacketHandler = std::function<void(const std::string &from,
                                             const std::string &raw_bytes,
                                             std::function<void(std::string &)> reply_cb
        )
    >;
    std::unordered_map<std::string, PacketHandler> handlers_;

    void handle_incoming_packet(const mesh::RoutedPacket &pkt);

    void setup_builtin_handlers();

    bool should_send_identity_request(const std::string &peer_id);

    void maybe_decrypt_packet(const mesh::RoutedPacket &pkt);

    void send_identity_request(const std::string &dest);

    void handle_pending_outgoing_packets(const std::string &dest);

    void handle_pending_incoming_packets(const std::string &src);
};


template<typename T>
    requires std::derived_from<T, google::protobuf::Message>
void MeshNode::send_message(const std::string &dest_id, const T &msg) {
    mesh::RoutedPacket pkt;
    std::string subtype = T::descriptor()->full_name();
    if (encrypt_messages_ && subtype != identity_request_subtype) {
        pkt = mesh::packet::MakeBinaryRoutedPacket(
            peer_id_, dest_id, MeshRouter::DEFAULT_TTL,
            subtype
        );

        if (!identity_->has_key(dest_id)) {
            Log::warn(
                "send_request_async",
                {{"dest_id", dest_id}},
                "missing dest_id public key, postponing send");
            identity_->buffer_outgoing_packet(dest_id, pkt);

            if (should_send_identity_request(dest_id)) {
                send_identity_request(dest_id);
            }
            return;
        }

        if (!security_->secure_packet(pkt, dest_id, msg.SerializeAsString())) {
            return;
        }
    } else {
        pkt = mesh::packet::MakeBinaryRoutedPacket(
            peer_id_, dest_id, MeshRouter::DEFAULT_TTL,
            subtype,
            msg.SerializeAsString()
        );
    }
    router_->send_packet(pkt);
}

template<typename T>
    requires std::derived_from<T, google::protobuf::Message>
void MeshNode::broadcast_message(const T &msg) {
    mesh::RoutedPacket pkt = mesh::packet::MakeBinaryRoutedPacket(
        peer_id_, "", MeshRouter::DEFAULT_TTL,
        T::descriptor()->full_name(),
        msg.SerializeAsString()
    );
    router_->send_packet(pkt);
}


template<typename RequestT, typename ResponseT>
    requires std::derived_from<ResponseT, google::protobuf::Message> &&
             std::derived_from<RequestT, google::protobuf::Message>
void MeshNode::send_request_async(
    const std::string &dest_id,
    const RequestT &req,
    std::function<void(const std::string &, std::expected<ResponseT, RequestError>)> callback,
    std::chrono::milliseconds timeout) {
    auto internal_cb = [callback = std::move(callback)
            ](const std::string &sender, std::optional<std::string> maybe_raw_bytes) {
        if (!maybe_raw_bytes.has_value()) {
            callback(sender, std::unexpected(RequestError::RequestTimeout));
            return;
        }

        const std::string &raw_bytes = maybe_raw_bytes.value();
        ResponseT resp;
        if (resp.ParseFromString(raw_bytes)) {
            callback(sender, resp);
        } else {
            Log::error("send_request_async", {{"from", sender}}, "failed to parse response");
            callback(sender, std::unexpected(RequestError::MalformedResponse));
        }
    };

    mesh::RoutedPacket pkt;
    std::string subtype = RequestT::descriptor()->full_name();
    if (encrypt_messages_ && subtype != identity_request_subtype) {
        pkt = mesh::packet::MakeBinaryRoutedPacket(
            peer_id_, dest_id, MeshRouter::DEFAULT_TTL,
            subtype,
            "",
            true
        );
        std::string req_id = to_hex(pkt.id());

        if (!identity_->has_key(dest_id)) {
            Log::warn(
                "send_request_async",
                {{"dest_id", dest_id}},
                "missing dest_id public key, postponing send");
            request_tracker_.track_request_callback(req_id, timeout, internal_cb);
            identity_->buffer_outgoing_packet(dest_id, pkt);

            if (should_send_identity_request(dest_id)) {
                send_identity_request(dest_id);
            }

            return;
        }

        if (!security_->secure_packet(pkt, dest_id, req.SerializeAsString())) {
            Log::error("send_request_async", {{"dest_id", dest_id}}, "encryption failed");
            callback(dest_id, std::unexpected(RequestError::EncryptionError));
            return;
        }
    } else {
        pkt = mesh::packet::MakeBinaryRoutedPacket(
            peer_id_, dest_id, MeshRouter::DEFAULT_TTL,
            subtype,
            req.SerializeAsString(),
            true
        );
    }

    std::string req_id = to_hex(pkt.id());
    request_tracker_.track_request_callback(req_id, timeout, internal_cb);
    router_->send_packet(pkt);;
}

template<typename RequestT, typename ResponseT>
    requires std::derived_from<ResponseT, google::protobuf::Message> &&
             std::derived_from<RequestT, google::protobuf::Message>
std::future<std::expected<std::pair<std::string, ResponseT>, RequestError> > MeshNode::send_request(
    const std::string &dest_id, const RequestT &req,
    std::chrono::milliseconds timeout) {
    auto prom = std::make_shared<std::promise<std::expected<std::pair<std::string, ResponseT>, RequestError> > >();

    send_request_async<RequestT, ResponseT>(
        dest_id, req,
        [prom](const std::string &peer, std::expected<ResponseT, RequestError> resp) {
            if (resp.has_value()) {
                prom->set_value(std::make_pair(peer, *resp));
            } else {
                prom->set_value(std::unexpected(resp.error()));
            }
        },
        timeout
    );

    return prom->get_future();
}


template<typename RequestT, typename ResponseT>
std::future<std::vector<std::pair<std::string, ResponseT> > > MeshNode::broadcast_request(
    const RequestT &req,
    std::chrono::milliseconds timeout) {
    // Shared state to collect responses
    auto results = std::make_shared<std::vector<std::pair<std::string, ResponseT> > >();
    auto prom = std::make_shared<std::promise<std::vector<std::pair<std::string, ResponseT> > > >();
    mesh::RoutedPacket pkt = mesh::packet::MakeBinaryRoutedPacket(
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

