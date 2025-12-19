#include "mesh_node.h"
#include <iostream>

#include "EnvelopeUtils.h"

MeshNode::MeshNode(boost::asio::io_context &ioc, const int tcp_port, const int udp_port, const std::string &peer_id)
    : ioc_(ioc), tcp_port_(tcp_port), udp_port_(udp_port), peer_id_(peer_id),
      acceptor_(ioc, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), tcp_port_)),
      discovery_(ioc, udp_port, tcp_port), rpc_connections(std::make_shared<RpcManager>(ioc, peer_id)) {
    std::lock_guard<std::mutex> guard(mu_);
    // for (auto &s: sessions_) {
    // }
}

void MeshNode::start() {
    do_accept();
    discovery_.start();
}

void MeshNode::stop() {
    boost::system::error_code ec;
    acceptor_.close(ec);
    discovery_.stop();
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
    std::optional<std::shared_ptr<RpcConnection> > maybe_conn = rpc_connections->get_connection(remote_id);
    if (!maybe_conn.has_value()) {
        std::cerr << "no conn found with remote_id " << remote_id << std::endl;
        return;
    }

    auto conn = maybe_conn.value();
    auto local_ip = mesh::envelope::MakePeerIP(conn->local_endpoint_);
    auto remote_ip = mesh::envelope::MakePeerIP(conn->remote_endpoint_);
    auto env = mesh::envelope::MakeCustomText(local_ip, remote_ip, text);
    auto fut = rpc_connections->send_message(remote_id, std::move(env));
    if (!fut.has_value()) {
        std::cerr << "send_message failed: " << fut.error() << std::endl;
    }
    fut->get();
}
