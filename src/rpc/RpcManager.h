#pragma once
#include <expected>
#include "IRpcMessageHandler.h"
#include "ITrasportLayer.h"
#include "RpcConnection.h"
#include "envelope.pb.h"
#include "IMessageSink.h"

namespace rpc {
    const int MAX_HEARTBEAT_FAILURES = 5;
}

class RpcManager : public std::enable_shared_from_this<RpcManager>, public ITransportLayer {
public:
    RpcManager(boost::asio::io_context &ioc, const std::string &peer_id,
               std::shared_ptr<IMessageSink> sink = nullptr);

    ~RpcManager();

    std::expected<std::string, std::string> create_connection(const std::string &remote_addr,
                                                              boost::asio::ip::tcp::socket sock);

    void accept_connection(const std::string &remote_addr,
                           boost::asio::ip::tcp::socket sock);

    bool remove_connection(std::string peer_id);

    std::optional<std::shared_ptr<RpcConnection> > get_connection(const std::string &node_id);

    std::expected<std::future<std::string>, SendError> send_message(const std::string &peer, mesh::Envelope &envelope,
                                                                    std::optional<std::chrono::milliseconds> timeout =
                                                                            std::nullopt);

    void send_heartbeats(std::chrono::milliseconds timeout);

    void set_sink(std::shared_ptr<IMessageSink> sink);

private:
    boost::asio::io_context &ioc_;
    const std::string &peer_id_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<RpcConnection> > connections_;
    std::thread heartbeat_thread_;
    std::unordered_map<mesh::EnvelopeType, std::unique_ptr<IRpcMessageHandler> > handlers_;
    std::weak_ptr<IMessageSink> sink_;

    void register_handlers();

    void dispatch_message(std::shared_ptr<RpcConnection> conn, const mesh::Envelope &envelope);
};
