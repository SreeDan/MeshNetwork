#pragma once
#include <boost/asio.hpp>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <vector>
#include "packet.pb.h"


class UdpTransport {
public:
    UdpTransport(boost::asio::io_context &ioc, uint16_t port);

    void send_packet(boost::asio::ip::udp::endpoint &dest_ep, const mesh::RoutedPacket &pkt);

    void set_on_receive(std::function<void(boost::asio::ip::udp::endpoint, std::string)> callback);

private:
    void start_receive();

    boost::asio::io_context &ioc_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remote_endpoint_;
    std::vector<char> recv_buffer_;

    std::function<void(boost::asio::ip::udp::endpoint, std::string)> on_receive_cb_;
};
