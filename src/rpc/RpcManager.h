#pragma once
#include <expected>
#include <boost/asio/io_service.hpp>

#include "RpcConnection.h"

enum SendError {
    INVALID_PEER,
};

class RpcManager : public std::enable_shared_from_this<RpcManager> {
public:
    RpcManager(boost::asio::io_context &ioc, const std::string &peer_id);

    ~RpcManager();

    std::expected<std::string, std::string> create_connection(const std::string &remote_addr,
                                                              boost::asio::ip::tcp::socket sock);

    void accept_connection(const std::string &remote_addr,
                           boost::asio::ip::tcp::socket sock);

    bool remove_connection(std::string peer_id);

    std::optional<std::shared_ptr<RpcConnection> > get_connection(const std::string &node_id);

    std::expected<std::future<std::string>, SendError> send_message(const std::string &peer, mesh::Envelope envelope,
                                                                    std::optional<std::chrono::milliseconds> timeout =
                                                                            std::nullopt);

private:
    boost::asio::io_context &ioc_;
    const std::string &peer_id_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<RpcConnection> > connections_;
    std::thread heartbeat_thread_;

    void send_heartbeats(std::chrono::milliseconds timeout);
};
