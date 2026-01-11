#include "Stream.h"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include "GraphManager.h"
#include "IStream.h"


TcpStream::TcpStream(boost::asio::ip::tcp::socket socket) : socket_(std::move(socket)) {
}

void TcpStream::async_handshake(std::function<void(boost::system::error_code)> handler) {
    // no-op for plain TCP
    post(socket_.get_executor(), [handler]() { handler({}); });
};

void TcpStream::async_read_fully(boost::asio::mutable_buffer buffer,
                                 std::function<void(boost::system::error_code, std::size_t)> handler) {
    boost::asio::async_read(socket_, buffer, handler);
}

void TcpStream::async_write_fully(boost::asio::const_buffer buffer,
                                  std::function<void(boost::system::error_code, std::size_t)> handler) {
    boost::asio::async_write(socket_, buffer, handler);
}

void TcpStream::cancel() {
    boost::system::error_code ec;
    socket_.cancel(ec);
}

void TcpStream::close() {
    boost::system::error_code ec;
    socket_.close(ec);
}

bool TcpStream::is_open() const { return socket_.is_open(); }


std::string TcpStream::remote_address() const {
    try {
        return socket_.remote_endpoint().address().to_string();
    } catch (...) {
        return "";
    }
}

SslStream::SslStream(boost::asio::ip::tcp::socket sock, boost::asio::ssl::context &ctx, bool is_server)
    : stream_(std::move(sock), ctx), is_server_(is_server) {
}

void SslStream::async_handshake(std::function<void(boost::system::error_code)> handler) {
    auto mode = is_server_ ? boost::asio::ssl::stream_base::server : boost::asio::ssl::stream_base::client;
    stream_.async_handshake(mode, handler);
}

void SslStream::async_read_fully(boost::asio::mutable_buffer buffer,
                                 std::function<void(boost::system::error_code, std::size_t)> handler) {
    boost::asio::async_read(stream_, buffer, handler);
}

void SslStream::async_write_fully(boost::asio::const_buffer buffer,
                                  std::function<void(boost::system::error_code, std::size_t)> handler) {
    boost::asio::async_write(stream_, buffer, handler);
}

void SslStream::close() {
    boost::system::error_code ec;
    stream_.lowest_layer().close(ec);
}

void SslStream::cancel() {
    boost::system::error_code ec;
    stream_.lowest_layer().cancel(ec);
}

bool SslStream::is_open() const {
    return stream_.lowest_layer().is_open();
}

std::string SslStream::remote_address() const {
    try {
        return stream_.lowest_layer().remote_endpoint().address().to_string();
    } catch (...) {
        return "";
    }
}
