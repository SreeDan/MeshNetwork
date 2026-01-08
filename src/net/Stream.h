#pragma once
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

#include "IStream.h"

class TcpStream : public StreamLayer {
public:
    explicit TcpStream(boost::asio::ip::tcp::socket socket);

    void async_handshake(std::function<void(boost::system::error_code)> handler) override;

    void async_read_fully(boost::asio::mutable_buffer buffer,
                          std::function<void(boost::system::error_code, std::size_t)> handler) override;

    void async_write_fully(boost::asio::const_buffer buffer,
                           std::function<void(boost::system::error_code, std::size_t)> handler) override;

    void cancel() override;

    void close() override;

    bool is_open() const override;

    std::string remote_address() const override;

private:
    boost::asio::ip::tcp::socket socket_;
};

class SslStream : public StreamLayer {
public:
    explicit SslStream(boost::asio::ip::tcp::socket sock, boost::asio::ssl::context &ctx, bool is_server);

    void async_handshake(std::function<void(boost::system::error_code)> handler) override;

    void async_read_fully(boost::asio::mutable_buffer buffer,
                          std::function<void(boost::system::error_code, std::size_t)> handler) override;

    void async_write_fully(boost::asio::const_buffer buffer,
                           std::function<void(boost::system::error_code, std::size_t)> handler) override;

    void cancel() override;

    void close() override;

    bool is_open() const override;

    std::string remote_address() const override;

private:
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream_;
    bool is_server_;
};
