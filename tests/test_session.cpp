#include <arpa/inet.h>

#include <array>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include <boost/asio/error.hpp>
#include <boost/asio/read.hpp>
#include <boost/uuid/random_generator.hpp>
#include <catch2/catch_test_macros.hpp>

#include "mesh/net/IStream.h"
#include "mesh/net/Session.h"
#include "mesh/utils/MessageUtils.h"

class MockStream : public StreamLayer {
public:
    using HandshakeCallback = std::function<void(boost::system::error_code)>;
    using IoCallback = std::function<void(boost::system::error_code, std::size_t)>;

    HandshakeCallback handshake_cb;
    IoCallback read_cb;
    boost::asio::mutable_buffer pending_read_buf;

    std::deque<IoCallback> write_cbs;
    std::vector<std::string> write_payloads;

    std::string remote_address_ = "127.0.0.1:mock";
    bool is_open_ = true;
    bool cancel_called = false;
    bool close_called = false;

    void async_handshake(HandshakeCallback cb) override {
        handshake_cb = std::move(cb);
    }

    void async_read_fully(boost::asio::mutable_buffer buffer, IoCallback cb) override {
        pending_read_buf = buffer;
        read_cb = std::move(cb);
    }

    void async_write_fully(boost::asio::const_buffer buffer, IoCallback cb) override {
        const auto* data = static_cast<const char*>(buffer.data());
        write_payloads.emplace_back(data, buffer.size());
        write_cbs.push_back(std::move(cb));
    }

    std::string remote_address() const override {
        return remote_address_;
    }

    bool is_open() const override { return is_open_; }

    void cancel() override { cancel_called = true; }

    void close() override {
        close_called = true;
        is_open_ = false;
    }

    void complete_next_write(boost::system::error_code ec = {}) {
        REQUIRE_FALSE(write_cbs.empty());
        auto cb = std::move(write_cbs.front());
        write_cbs.pop_front();
        cb(ec, write_payloads.front().size());
    }
};

namespace {

void pump(boost::asio::io_context& ioc) {
    ioc.restart();
    ioc.poll();
}

std::string make_dummy_uuid_str() {
    boost::uuids::uuid u = boost::uuids::random_generator()();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

uint32_t framed_payload_length(const std::string& frame) {
    uint32_t net_len = 0;
    std::memcpy(&net_len, frame.data() + 16, sizeof(net_len));
    return ntohl(net_len);
}

void write_header(boost::asio::mutable_buffer buffer, const boost::uuids::uuid& uuid, uint32_t payload_len) {
    auto* data = static_cast<char*>(buffer.data());
    const uint32_t net_len = htonl(payload_len);
    std::memcpy(data, uuid.data, 16);
    std::memcpy(data + 16, &net_len, sizeof(net_len));
}

}

TEST_CASE("GenericSession starts with handshake then begins header read", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    auto* mock = mock_stream.get();

    auto session = std::make_shared<GenericSession>(ioc, std::move(mock_stream),
                                                    [](const boost::uuids::uuid&, const std::string&) {});

    session->start();
    pump(ioc);

    REQUIRE(mock->handshake_cb != nullptr);
    REQUIRE(mock->read_cb == nullptr);

    mock->handshake_cb({});
    pump(ioc);

    REQUIRE(mock->read_cb != nullptr);
    REQUIRE(mock->pending_read_buf.size() == 20);
}

TEST_CASE("GenericSession stops when handshake fails", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    auto* mock = mock_stream.get();

    auto session = std::make_shared<GenericSession>(ioc, std::move(mock_stream),
                                                    [](const boost::uuids::uuid&, const std::string&) {});

    session->start();
    pump(ioc);

    mock->handshake_cb(boost::asio::error::operation_aborted);
    pump(ioc);

    REQUIRE(mock->cancel_called);
    REQUIRE(mock->close_called);
    REQUIRE_FALSE(mock->is_open());
}

TEST_CASE("GenericSession frames outgoing messages as UUID, big-endian length, payload", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    auto* mock = mock_stream.get();

    auto session = std::make_shared<GenericSession>(ioc, std::move(mock_stream),
                                                    [](const boost::uuids::uuid&, const std::string&) {});

    const std::string req_id = generate_uuid_bytes(DEFAULT_UUID_SIZE);
    const std::string payload = "Hello World";

    session->async_send_message(req_id, payload);
    pump(ioc);

    REQUIRE(mock->write_payloads.size() == 1);
    const auto& frame = mock->write_payloads.front();
    REQUIRE(frame.size() == DEFAULT_UUID_SIZE + sizeof(uint32_t) + payload.size());
    REQUIRE(frame.substr(0, DEFAULT_UUID_SIZE) == req_id);
    REQUIRE(framed_payload_length(frame) == payload.size());
    REQUIRE(frame.substr(20) == payload);
}

TEST_CASE("GenericSession serializes queued writes in order", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    auto* mock = mock_stream.get();

    auto session = std::make_shared<GenericSession>(ioc, std::move(mock_stream),
                                                    [](const boost::uuids::uuid&, const std::string&) {});

    session->async_send_message(make_dummy_uuid_str(), "first");
    session->async_send_message(make_dummy_uuid_str(), "second");
    pump(ioc);

    REQUIRE(mock->write_payloads.size() == 1);
    REQUIRE(mock->write_payloads[0].substr(20) == "first");

    mock->complete_next_write();
    pump(ioc);

    REQUIRE(mock->write_payloads.size() == 2);
    REQUIRE(mock->write_payloads[1].substr(20) == "second");
}

TEST_CASE("GenericSession stops and clears queued writes on write error", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    auto* mock = mock_stream.get();

    auto session = std::make_shared<GenericSession>(ioc, std::move(mock_stream),
                                                    [](const boost::uuids::uuid&, const std::string&) {});

    session->async_send_message(make_dummy_uuid_str(), "first");
    session->async_send_message(make_dummy_uuid_str(), "second");
    pump(ioc);

    REQUIRE(mock->write_payloads.size() == 1);

    mock->complete_next_write(boost::asio::error::operation_aborted);
    pump(ioc);

    REQUIRE(mock->cancel_called);
    REQUIRE(mock->close_called);
    REQUIRE(mock->write_payloads.size() == 1);
}

TEST_CASE("GenericSession reads payload, invokes handler, and loops to next header", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    auto* mock = mock_stream.get();

    bool message_received = false;
    boost::uuids::uuid received_id{};
    std::string received_payload;

    auto session = std::make_shared<GenericSession>(
        ioc,
        std::move(mock_stream),
        [&](const boost::uuids::uuid& id, const std::string& msg) {
            message_received = true;
            received_id = id;
            received_payload = msg;
        });

    session->start();
    pump(ioc);
    mock->handshake_cb({});
    pump(ioc);

    const boost::uuids::uuid id = boost::uuids::random_generator()();
    write_header(mock->pending_read_buf, id, 5);
    auto header_cb = std::move(mock->read_cb);
    header_cb({}, 20);
    pump(ioc);

    REQUIRE(mock->pending_read_buf.size() == 5);

    std::memcpy(mock->pending_read_buf.data(), "Hello", 5);
    auto payload_cb = std::move(mock->read_cb);
    payload_cb({}, 5);
    pump(ioc);

    REQUIRE(message_received);
    REQUIRE(received_id == id);
    REQUIRE(received_payload == "Hello");
    REQUIRE(mock->pending_read_buf.size() == 20);
    REQUIRE(mock->read_cb != nullptr);
}

TEST_CASE("GenericSession stops on oversized headers and read errors", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    auto* mock = mock_stream.get();

    auto session = std::make_shared<GenericSession>(ioc, std::move(mock_stream),
                                                    [](const boost::uuids::uuid&, const std::string&) {});

    SECTION("oversized payload length") {
        session->start();
        pump(ioc);
        mock->handshake_cb({});
        pump(ioc);

        const boost::uuids::uuid id = boost::uuids::random_generator()();
        write_header(mock->pending_read_buf, id, 11 * 1024 * 1024);
        mock->read_cb({}, 20);
        pump(ioc);

        REQUIRE(mock->cancel_called);
        REQUIRE(mock->close_called);
    }

    SECTION("header read error") {
        session->start();
        pump(ioc);
        mock->handshake_cb({});
        pump(ioc);

        mock->read_cb(boost::asio::error::connection_reset, 0);
        pump(ioc);

        REQUIRE(mock->cancel_called);
        REQUIRE(mock->close_called);
    }

    SECTION("payload read error") {
        session->start();
        pump(ioc);
        mock->handshake_cb({});
        pump(ioc);

        const boost::uuids::uuid id = boost::uuids::random_generator()();
        write_header(mock->pending_read_buf, id, 5);
        auto header_cb = std::move(mock->read_cb);
        header_cb({}, 20);
        pump(ioc);

        mock->read_cb(boost::asio::error::connection_reset, 0);
        pump(ioc);

        REQUIRE(mock->cancel_called);
        REQUIRE(mock->close_called);
    }
}

TEST_CASE("GenericSession reports remote address", "[session]") {
    boost::asio::io_context ioc;
    auto mock_stream = std::make_unique<MockStream>();
    mock_stream->remote_address_ = "10.0.0.5:12345";

    auto session = std::make_shared<GenericSession>(ioc, std::move(mock_stream),
                                                    [](const boost::uuids::uuid&, const std::string&) {});

    REQUIRE(session->remote_addr() == "10.0.0.5:12345");
}

TEST_CASE("GenericSession E2E: Session to Session", "[session][integration]") {
    boost::asio::io_context ioc;

    boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::ip::tcp::socket server_sock(ioc);
    boost::asio::ip::tcp::socket client_sock(ioc);

    acceptor.async_accept(server_sock, [&](boost::system::error_code ec) {
        REQUIRE(!ec);
    });
    client_sock.async_connect(acceptor.local_endpoint(), [&](boost::system::error_code ec) {
        REQUIRE(!ec);
    });

    ioc.run_for(std::chrono::milliseconds(50));
    ioc.restart();

    REQUIRE(server_sock.is_open());
    REQUIRE(client_sock.is_open());

    std::string server_received_msg;
    bool server_got_msg = false;

    auto server_session = make_tcp_session(ioc, std::move(server_sock),
                                           [&](const boost::uuids::uuid&, const std::string& msg) {
                                               server_received_msg = msg;
                                               server_got_msg = true;
                                           });

    std::string client_received_msg;
    bool client_got_msg = false;

    auto client_session = make_tcp_session(ioc, std::move(client_sock),
                                           [&](const boost::uuids::uuid&, const std::string& msg) {
                                               client_received_msg = msg;
                                               client_got_msg = true;
                                           });

    server_session->start();
    client_session->start();

    client_session->async_send_message(make_dummy_uuid_str(), "Hello Server");
    ioc.run_for(std::chrono::milliseconds(50));
    ioc.restart();

    REQUIRE(server_got_msg);
    REQUIRE(server_received_msg == "Hello Server");

    server_session->async_send_message(make_dummy_uuid_str(), "I got your message");
    ioc.run_for(std::chrono::milliseconds(50));
    ioc.restart();

    REQUIRE(client_got_msg);
    REQUIRE(client_received_msg == "I got your message");

    client_session->stop();
    server_session->stop();
}
