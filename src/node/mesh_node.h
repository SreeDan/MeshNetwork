#pragma once
#include <string>
#include <boost/asio.hpp>
#include "net/session.h"


class MeshNode {
public:
    MeshNode(boost::asio::io_context& ioc, int tcp_port, int udp_port, const std::string& peer_id);
    void start();
    void stop();
private:
    boost::asio::io_context& ioc_;
    int tcp_port_;
    int udp_port_;
    std::string peer_id_;

    boost::asio::ip::tcp::acceptor acceptor_;

    std::vector<std::shared_ptr<ISession>> sessions_;
    std::mutex mu_;

    void do_accept();

};

