#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/net/udp/FecCodec.h"
#include "mesh/net/udp/UdpTransport.h"
#include "packet.pb.h"

namespace {

mesh::RoutedPacket make_packet() {
    mesh::RoutedPacket packet;
    packet.set_id("packet-id");
    packet.set_from_peer_id("sree");
    packet.set_to_peer_id("dan");
    packet.set_ttl(7);
    packet.set_type(mesh::TEXT);
    packet.set_transport(mesh::UDP);
    packet.set_subtype("test.message");
    packet.set_text("hello over udp");
    return packet;
}

mesh::RoutedPacket make_packet_with_id(const std::string& id, const std::string& text) {
    auto packet = make_packet();
    packet.set_id(id);
    packet.set_text(text);
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

}

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
    REQUIRE(parsed.from_peer_id() == "sree");
    REQUIRE(parsed.to_peer_id() == "dan");
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

TEST_CASE("UdpTransport sends FEC-framed datagrams when enabled", "[udp][transport][integration][fec]") {
    boost::asio::io_context ioc;
    UdpTransport sender(ioc, 0, UdpFecOptions{.enabled = true, .data_shards = 2, .roots = 1});

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
    REQUIRE(FecCodec::is_fec_frame(received_bytes));
    REQUIRE(received_bytes != expected_bytes);

    sender.shutdown();
    receiver.close();
}

TEST_CASE("UdpTransport FEC receiver still accepts raw UDP datagrams", "[udp][transport][integration][fec]") {
    boost::asio::io_context ioc;
    const auto receive_port = reserve_udp_port(ioc);
    UdpTransport receiver(ioc, receive_port, UdpFecOptions{.enabled = true, .data_shards = 2, .roots = 1});

    boost::asio::ip::udp::socket sender(ioc, {boost::asio::ip::udp::v4(), 0});
    bool callback_called = false;
    std::string callback_payload;

    receiver.set_on_receive([&](boost::asio::ip::udp::endpoint, std::string bytes) {
        callback_called = true;
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
    REQUIRE(callback_payload == payload);

    receiver.shutdown();
    sender.close();
}

TEST_CASE("UdpTransport decodes FEC datagrams into original UDP payloads", "[udp][transport][integration][fec]") {
    boost::asio::io_context ioc;
    const auto receive_port = reserve_udp_port(ioc);
    UdpTransport sender(ioc, 0, UdpFecOptions{.enabled = true, .data_shards = 2, .roots = 1});
    UdpTransport receiver(ioc, receive_port, UdpFecOptions{.enabled = true, .data_shards = 2, .roots = 1});

    std::vector<std::string> received_payloads;
    receiver.set_on_receive([&](boost::asio::ip::udp::endpoint, std::string bytes) {
        received_payloads.push_back(std::move(bytes));
    });

    auto packet1 = make_packet_with_id("packet-1", "first fec packet");
    auto packet2 = make_packet_with_id("packet-2", "second fec packet");
    const auto expected1 = packet1.SerializeAsString();
    const auto expected2 = packet2.SerializeAsString();

    auto dest = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), receive_port);
    sender.send_packet(dest, packet1);
    sender.send_packet(dest, packet2);

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return received_payloads.size() == 2; }));
    REQUIRE(received_payloads[0] == expected1);
    REQUIRE(received_payloads[1] == expected2);

    sender.shutdown();
    receiver.shutdown();
}

TEST_CASE("UdpTransport default receiver auto-detects FEC datagrams", "[udp][transport][integration][fec]") {
    boost::asio::io_context ioc;
    const auto receive_port = reserve_udp_port(ioc);
    UdpTransport sender(ioc, 0, UdpFecOptions{.enabled = true, .data_shards = 2, .roots = 1});
    UdpTransport receiver(ioc, receive_port);

    std::vector<std::string> received_payloads;
    receiver.set_on_receive([&](boost::asio::ip::udp::endpoint, std::string bytes) {
        received_payloads.push_back(std::move(bytes));
    });

    auto packet1 = make_packet_with_id("packet-1", "first fec packet");
    auto packet2 = make_packet_with_id("packet-2", "second fec packet");
    const auto expected1 = packet1.SerializeAsString();
    const auto expected2 = packet2.SerializeAsString();

    auto dest = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::loopback(), receive_port);
    sender.send_packet(dest, packet1);
    sender.send_packet(dest, packet2);

    REQUIRE(run_for(ioc, std::chrono::milliseconds(250), [&] { return received_payloads.size() == 2; }));
    REQUIRE(received_payloads[0] == expected1);
    REQUIRE(received_payloads[1] == expected2);

    sender.shutdown();
    receiver.shutdown();
}

TEST_CASE("UdpTransport shutdown is idempotent", "[udp][transport][integration]") {
    boost::asio::io_context ioc;
    UdpTransport transport(ioc, 0);

    REQUIRE_NOTHROW(transport.shutdown());
    REQUIRE_NOTHROW(transport.shutdown());
}
