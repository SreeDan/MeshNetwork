#include "UdpTransport.h"

#include <iostream>

#include "Logger.h"

UdpTransport::UdpTransport(boost::asio::io_context &ioc, uint16_t port)
    : ioc_(ioc),
      socket_(ioc, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port)),
      recv_buffer_(65536) {
    start_receive();
}


void UdpTransport::send_packet(boost::asio::ip::udp::endpoint &dest_ep, const mesh::RoutedPacket &pkt) {
    std::string payload = pkt.SerializeAsString();

    // We duplicate payload into a shared_ptr or string to keep it alive for the async op
    auto buffer_ptr = std::make_shared<std::string>(std::move(payload));

    socket_.async_send_to(
        boost::asio::buffer(*buffer_ptr),
        dest_ep,
        [dest_ep, buffer_ptr](boost::system::error_code ec, std::size_t /*bytes*/) {
            if (ec) {
                Log::warn("udp_send_packet",
                          {{"ec", ec.to_string()}, {"dest", dest_ep.address().to_string()}, {"port", dest_ep.port()}},
                          "failed to send udp buffer");
            }
        }
    );
}

void UdpTransport::set_on_receive(std::function<void(boost::asio::ip::udp::endpoint, std::string)> callback) {
    on_receive_cb_ = callback;
}

void UdpTransport::start_receive() {
    socket_.async_receive_from(
        boost::asio::buffer(recv_buffer_),
        remote_endpoint_,
        [this](boost::system::error_code ec, std::size_t bytes_recvd) {
            if (!ec && bytes_recvd > 0) {
                std::string raw_data(recv_buffer_.data(), bytes_recvd);
                if (on_receive_cb_) {
                    on_receive_cb_(remote_endpoint_, raw_data);
                }
            }

            start_receive();
        }
    );
}
