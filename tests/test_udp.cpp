#include <array>
#include <chrono>
#include <functional>
#include <string>

#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/net/udp/UdpTransport.h"
#include "packet.pb.h"

namespace {

mesh::RoutedPacket make_packet() {
    mesh::RoutedPacket packet;
    packet.set_id("packet-id");
    packet.set_from_peer_id("alice");
    packet.set_to_peer_id("bob");
    packet.set_ttl(7);
    packet.set_type(mesh::TEXT);
    packet.set_transport(mesh::UDP);
    packet.set_subtype("test.message");
    packet.set_text("hello over udp");
    return packet;
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

uint16_t reserve_udp_port(boost::asio::io_context& ioc) {
    boost::asio::ip::udp::socket socket(ioc, {boost::asio::ip::udp::v4(), 0});
    const auto port = socket.local_endpoint().port();
    socket.close();
    return port;
}

} // namespace

TEST_CASE("UdpTransport sends serialized RoutedPacket datagrams", "[udp][transport][integration]") {
    boost::asio::io_context ioc;
    UdpTransport sender(ioc, 0);

    boost::asio::ip::udp::socket receiver(ioc, {boost::asio::ip::udp::v4(), 0});
    boost::asio::ip::udp::endpoint sender_endpoint;
    std::array<char, 2048> recv_buffer{};
    std::string received_bytes;

    receiver.async_receive_from(
        boost::asio::buffer(recv_buffer),
        sender_endpoint,
        [&](boost::system::error_code ec, std::size_t bytes) {
            REQUIRE_FALSE(ec);
            received_bytes.assign(recv_buffer.data(), bytes);
        });

    auto packet = make_packet();
    const auto expected_bytes = packet.SerializeAsString();
    auto dest = receiver.local_endpoint();
    sender.send_packet(dest, packet);

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return !received_bytes.empty(); }));
    REQUIRE(received_bytes == expected_bytes);

    mesh::RoutedPacket parsed;
    REQUIRE(parsed.ParseFromString(received_bytes));
    REQUIRE(parsed.from_peer_id() == "alice");
    REQUIRE(parsed.to_peer_id() == "bob");
    REQUIRE(parsed.ttl() == 7);
    REQUIRE(parsed.transport() == mesh::UDP);
    REQUIRE(parsed.text() == "hello over udp");

    sender.shutdown();
    receiver.close();
}

TEST_CASE("UdpTransport receive callback exposes sender endpoint and raw bytes", "[udp][transport][integration]") {
    boost::asio::io_context ioc;
    const auto receive_port = reserve_udp_port(ioc);
    UdpTransport receiver(ioc, receive_port);

    boost::asio::ip::udp::socket sender(ioc, {boost::asio::ip::udp::v4(), 0});
    const auto sender_port = sender.local_endpoint().port();

    bool callback_called = false;
    boost::asio::ip::udp::endpoint callback_endpoint;
    std::string callback_payload;

    receiver.set_on_receive([&](boost::asio::ip::udp::endpoint endpoint, std::string bytes) {
        callback_called = true;
        callback_endpoint = endpoint;
        callback_payload = std::move(bytes);
    });

    const std::string payload = "raw udp payload";
    sender.async_send_to(
        boost::asio::buffer(payload),
        {boost::asio::ip::address_v4::loopback(), receive_port},
        [](boost::system::error_code ec, std::size_t bytes) {
            REQUIRE_FALSE(ec);
            REQUIRE(bytes == std::string("raw udp payload").size());
        });

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return callback_called; }));
    REQUIRE(callback_endpoint.port() == sender_port);
    REQUIRE(callback_payload == payload);

    receiver.shutdown();
    sender.close();
}

TEST_CASE("UdpTransport shutdown is idempotent", "[udp][transport][integration]") {
    boost::asio::io_context ioc;
    UdpTransport transport(ioc, 0);

    REQUIRE_NOTHROW(transport.shutdown());
    REQUIRE_NOTHROW(transport.shutdown());
}
