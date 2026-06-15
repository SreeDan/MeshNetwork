#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <vector>
#include "mesh/net/udp/FecCodec.h"
#include "packet.pb.h"

struct UdpFecOptions {
    bool enabled{false};
    uint8_t data_shards{4};
    uint8_t roots{0};
};

class UdpTransport {
public:
    UdpTransport(boost::asio::io_context &ioc, uint16_t port, UdpFecOptions fec_options = {});

    void send_packet(boost::asio::ip::udp::endpoint &dest_ep, const mesh::RoutedPacket &pkt);

    void shutdown();

    void set_on_receive(std::function<void(boost::asio::ip::udp::endpoint, std::string)> callback);

private:
    void start_receive();
    void send_datagram(boost::asio::ip::udp::endpoint dest_ep, std::string datagram);
    FecCodec& encoder_for(const boost::asio::ip::udp::endpoint& endpoint);
    FecCodec& decoder_for(const boost::asio::ip::udp::endpoint& endpoint, uint8_t data_shards, uint8_t roots);
    static std::string endpoint_key(const boost::asio::ip::udp::endpoint& endpoint);

    boost::asio::io_context &ioc_;
    boost::asio::ip::udp::socket socket_;
    boost::asio::ip::udp::endpoint remote_endpoint_;
    std::vector<char> recv_buffer_;
    UdpFecOptions fec_options_;
    std::unordered_map<std::string, std::unique_ptr<FecCodec>> encoders_by_endpoint_;
    std::unordered_map<std::string, std::unique_ptr<FecCodec>> decoders_by_endpoint_;

    std::function<void(boost::asio::ip::udp::endpoint, std::string)> on_receive_cb_;
};
