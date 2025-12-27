#pragma once
#include <string>
#include <boost/asio.hpp>

#include "discovery.h"
#include "RpcConnection.h"
#include "RpcManager.h"
#include "net/session.h"


class MeshNode {
public:
    MeshNode(boost::asio::io_context &ioc, int tcp_port, int udp_port, const std::string &peer_id);

    void start();

    void stop();

    void send_message(const std::string &peer_id, const std::string &text);

    void connect_to(const std::string &host, int port);

private:
    boost::asio::io_context &ioc_;
    int tcp_port_;
    int udp_port_;
    std::string peer_id_;

    boost::asio::ip::tcp::acceptor acceptor_;

    std::shared_ptr<RpcManager> rpc_connections;

    std::mutex mu_;

    void do_accept();
};

