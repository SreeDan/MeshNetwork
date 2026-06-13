#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/net/Stream.h"

namespace {

using boost::asio::ip::tcp;

void pump(boost::asio::io_context& ioc) {
    ioc.restart();
    ioc.poll();
}

bool run_for(boost::asio::io_context& ioc, std::chrono::milliseconds timeout,
             const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        ioc.run_for(std::chrono::milliseconds(5));
        ioc.restart();
    }
    return done();
}

std::pair<tcp::socket, tcp::socket> make_connected_sockets(boost::asio::io_context& ioc) {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::address_v4::loopback(), 0});
    tcp::socket server_socket(ioc);
    tcp::socket client_socket(ioc);

    bool accepted = false;
    bool connected = false;

    acceptor.async_accept(server_socket, [&](boost::system::error_code ec) {
        REQUIRE_FALSE(ec);
        accepted = true;
    });
    client_socket.async_connect(acceptor.local_endpoint(), [&](boost::system::error_code ec) {
        REQUIRE_FALSE(ec);
        connected = true;
    });

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return accepted && connected; }));
    acceptor.close();

    return {std::move(client_socket), std::move(server_socket)};
}

}

TEST_CASE("TcpStream handshake completes as posted no-op", "[stream][tcp][integration]") {
    boost::asio::io_context ioc;
    auto [client_socket, server_socket] = make_connected_sockets(ioc);
    TcpStream stream(std::move(client_socket));

    bool handshake_called = false;
    boost::system::error_code handshake_ec;

    stream.async_handshake([&](boost::system::error_code ec) {
        handshake_called = true;
        handshake_ec = ec;
    });

    REQUIRE_FALSE(handshake_called);
    pump(ioc);

    REQUIRE(handshake_called);
    REQUIRE_FALSE(handshake_ec);

    stream.close();
    server_socket.close();
}

TEST_CASE("TcpStream writes a complete buffer to its peer", "[stream][tcp][integration]") {
    boost::asio::io_context ioc;
    auto [client_socket, server_socket] = make_connected_sockets(ioc);
    TcpStream stream(std::move(client_socket));

    const std::string outgoing = "complete tcp stream write";
    std::array<char, 128> read_buffer{};
    std::string received;
    bool write_done = false;
    bool read_done = false;

    boost::asio::async_read(
        server_socket,
        boost::asio::buffer(read_buffer.data(), outgoing.size()),
        [&](boost::system::error_code ec, std::size_t bytes) {
            REQUIRE_FALSE(ec);
            received.assign(read_buffer.data(), bytes);
            read_done = true;
        });

    stream.async_write_fully(
        boost::asio::buffer(outgoing),
        [&](boost::system::error_code ec, std::size_t bytes) {
            REQUIRE_FALSE(ec);
            REQUIRE(bytes == outgoing.size());
            write_done = true;
        });

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return write_done && read_done; }));
    REQUIRE(received == outgoing);

    stream.close();
    server_socket.close();
}

TEST_CASE("TcpStream reads the requested byte count across peer chunks", "[stream][tcp][integration]") {
    boost::asio::io_context ioc;
    auto [client_socket, server_socket] = make_connected_sockets(ioc);
    TcpStream stream(std::move(client_socket));

    std::array<char, 10> read_buffer{};
    bool read_done = false;
    std::string received;

    stream.async_read_fully(
        boost::asio::buffer(read_buffer),
        [&](boost::system::error_code ec, std::size_t bytes) {
            REQUIRE_FALSE(ec);
            received.assign(read_buffer.data(), bytes);
            read_done = true;
        });

    const std::string first_chunk = "abc";
    bool first_write_done = false;
    boost::asio::async_write(
        server_socket,
        boost::asio::buffer(first_chunk),
        [&](boost::system::error_code ec, std::size_t bytes) {
            REQUIRE_FALSE(ec);
            REQUIRE(bytes == first_chunk.size());
            first_write_done = true;
        });

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return first_write_done; }));
    REQUIRE_FALSE(read_done);

    const std::string second_chunk = "defghij";
    bool second_write_done = false;
    boost::asio::async_write(
        server_socket,
        boost::asio::buffer(second_chunk),
        [&](boost::system::error_code ec, std::size_t bytes) {
            REQUIRE_FALSE(ec);
            REQUIRE(bytes == second_chunk.size());
            second_write_done = true;
        });

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return second_write_done && read_done; }));
    REQUIRE(received == first_chunk + second_chunk);

    stream.close();
    server_socket.close();
}

TEST_CASE("TcpStream reports remote address and socket lifecycle", "[stream][tcp][integration]") {
    boost::asio::io_context ioc;
    auto [client_socket, server_socket] = make_connected_sockets(ioc);
    TcpStream stream(std::move(client_socket));

    REQUIRE(stream.is_open());
    REQUIRE(stream.remote_address() == "127.0.0.1");

    stream.cancel();
    REQUIRE(stream.is_open());

    stream.close();
    REQUIRE_FALSE(stream.is_open());
    REQUIRE(stream.remote_address().empty());

    server_socket.close();
}
