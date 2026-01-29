#pragma once
#include <expected>
#include "IRpcMessageHandler.h"
#include "ITrasportLayer.h"
#include "RpcConnection.h"
#include "envelope.pb.h"
#include "IMessageSink.h"
#include "UdpTransport.h"

namespace rpc {
    const int MAX_HEARTBEAT_FAILURES = 5;
}

class RpcManager : public std::enable_shared_from_this<RpcManager>, public ITransportLayer {
public:
    RpcManager(boost::asio::io_context &ioc,
               const std::string &peer_id,
               int tcp_port,
               int udp_port,
               std::shared_ptr<IMessageSink> sink = nullptr,
               std::shared_ptr<boost::asio::ssl::context> ssl_ctx = nullptr);

    ~RpcManager();

    void start_listening();

    void shutdown();

    void set_sink(const std::shared_ptr<IMessageSink> &sink);

    std::expected<std::string, std::string> connect(const std::string &host, int port);

    void add_auto_connection(const mesh::PeerIP &record);

    void remove_auto_connection(const mesh::PeerIP &record);

    std::expected<std::future<std::string>, SendError> send_tcp_message(
        const std::string &peer, mesh::Envelope &envelope,
        std::optional<std::chrono::milliseconds> timeout =
                std::nullopt);

    std::expected<void, SendError> send_udp_message(const std::string &peer, mesh::RoutedPacket &pkt);

    std::optional<std::shared_ptr<RpcConnection> > get_connection(const std::string &node_id);

    bool remove_connection(const std::string &peer_id);

private:
    boost::asio::io_context &ioc_;
    const std::string &peer_id_;
    int tcp_port_;
    int udp_port_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::weak_ptr<IMessageSink> sink_;
    std::shared_ptr<UdpTransport> udp_transport_;
    std::shared_ptr<boost::asio::ssl::context> ssl_ctx_;
    boost::asio::steady_timer maintenance_timer_{ioc_};

    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<RpcConnection> > connections_by_peer_;
    std::unordered_map<std::string, std::shared_ptr<RpcConnection> > connections_by_ip_;

    std::vector<mesh::PeerIP> auto_connections_;

    std::thread heartbeat_thread_;
    std::unordered_map<mesh::EnvelopeType, std::unique_ptr<IRpcMessageHandler> > handlers_;

    void register_handlers();

    std::expected<std::string, std::string> handle_new_connection(boost::asio::ip::tcp::socket socket, bool initiator);

    void dispatch_message(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &envelope);

    void add_connection_internal(const std::string &peer_id, std::shared_ptr<RpcConnection> conn);

    void run_maintenance_cycle();

    void check_for_auto_connections_locked();

    void send_heartbeats(std::chrono::milliseconds timeout);

    void do_accept();

    std::expected<std::string, std::string> handle_connection_startup(std::shared_ptr<RpcConnection> conn,
                                                                      bool initiator);
};
