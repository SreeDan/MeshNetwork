#include "mesh_node.h"
#include <iostream>

MeshNode::MeshNode(boost::asio::io_context &ioc, const int tcp_port, const int udp_port, const std::string &peer_id)
    : ioc_(ioc), tcp_port_(tcp_port), udp_port_(udp_port), peer_id_(peer_id),
      acceptor_(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), tcp_port_)) {
    std::lock_guard<std::mutex> g(mu_);
}

void MeshNode::start() {
    do_accept();
}

void MeshNode::stop() {
}

void MeshNode::do_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket sock) {
        if (!ec) {
            std::cout << "Incoming connection from " << sock.remote_endpoint().address().to_string() << ":" << sock.
                    remote_endpoint().port() << std::endl;
            // std::shared_ptr<ISession> sess = make_plain_session(ioc_, std::move(sock),
            //                                                     [this](std::shared_ptr<ISession> s) {
            //                                                     });
        }
    });
}
