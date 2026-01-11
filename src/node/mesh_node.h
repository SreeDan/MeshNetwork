#pragma once
#include <string>
#include <boost/asio.hpp>

#include "discovery.h"
#include "MeshRouter.h"
#include "RpcConnection.h"
#include "RpcManager.h"
#include "topology.pb.h"
#include "net/session.h"


class MeshNode {
public:
    MeshNode(boost::asio::io_context &ioc, int tcp_port, int udp_port, const std::string &peer_id);

    void start();

    void stop();

    void send_message(const std::string &peer_id, const std::string &text);

    void connect_to(const std::string &host, int port);

    void handle_received_message(const std::string &from, const mesh::RoutedPacket &pkt);

    void set_output_directory(const std::string &dir);

    void generate_topology_graph(const std::string &filename);

    template<typename T>
    // A callback that handles a request
    // - from_peer: who sent the message
    // - payload: the request data
    // - reply_cb: a function the handler must call if it wants to send a response
    using TypedMessageHandler = std::function<void(
            const std::string &from_peer,
            const T &msg,
            std::function<void(std::string &)>
            reply_cb
        )
    >;

    void send_text(const std::string &peer_id, const std::string &text);

    template<typename T>
    void send(const std::string &dest_id, const T &msg);

    template<typename ResponseT, typename RequestT>
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

private:
    using TypeErasedHandler = std::function<void(const std::string &from,
                                                 const std::string &raw_bytes,
                                                 std::function<void(std::string &)> reply_cb
        )
    >;

    boost::asio::io_context &ioc_;
    boost::asio::ip::tcp::acceptor acceptor_;
    int tcp_port_;
    int udp_port_;
    std::string peer_id_;
    std::string output_directory_;

    std::shared_ptr<RpcManager> rpc_connections;
    std::shared_ptr<MeshRouter> router_;

    std::unordered_map<std::string, TypeErasedHandler> handlers_;

    void dispatch_message(const std::string &from, const mesh::RoutedPacket &pkt);

    void enable_topology_features();

    void do_accept();
};
