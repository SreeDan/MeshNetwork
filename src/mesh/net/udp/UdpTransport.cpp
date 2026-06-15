#include "mesh/net/udp/UdpTransport.h"

#include <iostream>
#include <stdexcept>

#include "mesh/logging/Logger.h"

UdpTransport::UdpTransport(boost::asio::io_context &ioc, uint16_t port, UdpFecOptions fec_options)
    : ioc_(ioc),
      socket_(ioc, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port)),
      recv_buffer_(65536),
      fec_options_(fec_options) {
    if (fec_options_.roots == 0) {
        fec_options_.enabled = false;
    }
    if (fec_options_.enabled) {
        if (fec_options_.data_shards < 1) {
            throw std::invalid_argument("UDP FEC requires at least one data shard");
        }
        if (static_cast<int>(fec_options_.data_shards) + fec_options_.roots > 254) {
            throw std::invalid_argument("UDP FEC requires data shards + roots <= 254");
        }
    }
    start_receive();
}


void UdpTransport::send_packet(boost::asio::ip::udp::endpoint &dest_ep, const mesh::RoutedPacket &pkt) {
    if (!socket_.is_open()) {
        return;
    }

    std::string payload = pkt.SerializeAsString();

    if (!fec_options_.enabled) {
        send_datagram(dest_ep, std::move(payload));
        return;
    }

    try {
        auto& encoder = encoder_for(dest_ep);
        auto encoded = encoder.encode(payload);
        send_datagram(dest_ep, std::move(encoded.data_frame));
        for (auto& parity_frame : encoded.parity_frames) {
            send_datagram(dest_ep, std::move(parity_frame));
        }
    } catch (const std::exception& e) {
        Log::warn("udp_send_packet",
                  {{"dest", dest_ep.address().to_string()}, {"port", dest_ep.port()}, {"error", e.what()}},
                  "failed to encode udp packet with fec");
    }
}

void UdpTransport::send_datagram(boost::asio::ip::udp::endpoint dest_ep, std::string datagram) {
    if (!socket_.is_open()) {
        return;
    }

    auto buffer_ptr = std::make_shared<std::string>(std::move(datagram));

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

void UdpTransport::shutdown() {
    boost::system::error_code ec;
    socket_.cancel(ec);
    socket_.close(ec);
}

void UdpTransport::set_on_receive(std::function<void(boost::asio::ip::udp::endpoint, std::string)> callback) {
    on_receive_cb_ = callback;
}

void UdpTransport::start_receive() {
    if (!socket_.is_open()) {
        return;
    }

    socket_.async_receive_from(
        boost::asio::buffer(recv_buffer_),
        remote_endpoint_,
        [this](boost::system::error_code ec, std::size_t bytes_recvd) {
            if (ec == boost::asio::error::operation_aborted || !socket_.is_open()) {
                return;
            }

            if (!ec && bytes_recvd > 0) {
                std::string raw_data(recv_buffer_.data(), bytes_recvd);
                if (on_receive_cb_) {
                    if (FecCodec::is_fec_frame(raw_data)) {
                        try {
                            auto config = FecCodec::frame_config(raw_data);
                            if (!config) {
                                start_receive();
                                return;
                            }
                            auto& decoder = decoder_for(remote_endpoint_, config->first, config->second);
                            for (auto& payload : decoder.decode(raw_data)) {
                                on_receive_cb_(remote_endpoint_, std::move(payload));
                            }
                        } catch (const std::exception& e) {
                            Log::warn("udp_receive_packet",
                                      {{"src", remote_endpoint_.address().to_string()},
                                       {"port", remote_endpoint_.port()},
                                       {"error", e.what()}},
                                      "failed to decode udp fec frame");
                        }
                    } else {
                        on_receive_cb_(remote_endpoint_, raw_data);
                    }
                }
            }

            start_receive();
        }
    );
}

FecCodec& UdpTransport::encoder_for(const boost::asio::ip::udp::endpoint& endpoint) {
    const auto key = endpoint_key(endpoint);
    auto& codec = encoders_by_endpoint_[key];
    if (!codec) {
        codec = std::make_unique<FecCodec>(fec_options_.data_shards, fec_options_.roots);
    }
    return *codec;
}

FecCodec& UdpTransport::decoder_for(const boost::asio::ip::udp::endpoint& endpoint,
                                     uint8_t data_shards,
                                     uint8_t roots) {
    const auto key = endpoint_key(endpoint) + "/" + std::to_string(data_shards) + "/" + std::to_string(roots);
    auto& codec = decoders_by_endpoint_[key];
    if (!codec) {
        codec = std::make_unique<FecCodec>(data_shards, roots);
    }
    return *codec;
}

std::string UdpTransport::endpoint_key(const boost::asio::ip::udp::endpoint& endpoint) {
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}
