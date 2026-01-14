#include <boost/asio/read.hpp>
#include <boost/uuid/random_generator.hpp>
#include <catch2/catch_test_macros.hpp>

#include "IStream.h"
#include "message_utils.h"
#include "Session.h"

class MockStream : public StreamLayer {
public:
    std::function<void(boost::system::error_code)> handshake_cb;

    std::function<void(boost::system::error_code, std::size_t)> read_cb;
    boost::asio::mutable_buffer pending_read_buf;

    std::function<void(boost::system::error_code, std::size_t)> write_cb;
    boost::asio::const_buffer pending_write_buf;

    bool is_open_ = true;
    bool cancel_called = false;
    bool close_called = false;

    void async_handshake(std::function<void(boost::system::error_code)> cb) override {
        handshake_cb = std::move(cb);
    }

    void async_read_fully(boost::asio::mutable_buffer buffer,
                          std::function<void(boost::system::error_code, std::size_t)> cb) override {
        pending_read_buf = buffer;
        read_cb = std::move(cb);
    }

    void async_write_fully(boost::asio::const_buffer buffer,
                           std::function<void(boost::system::error_code, std::size_t)> cb) override {
        pending_write_buf = buffer;
        write_cb = std::move(cb);
    }

    std::string remote_address() const override {
        return "127.0.0.1:mock";
    }

    bool is_open() const override { return is_open_; }

    void cancel() override { cancel_called = true; }

    void close() override {
        close_called = true;
        is_open_ = false;
    }
};

std::string make_dummy_uuid_str() {
    boost::uuids::uuid u = boost::uuids::random_generator()();
    return std::string(reinterpret_cast<const char *>(u.data()), u.size());
}


TEST_CASE("GenericSession Unit Logic", "[session]") {
    boost::asio::io_context ioc;

    auto mock_stream_ptr = std::make_unique<MockStream>();
    // just use the raw pointer to manipulate it
    MockStream *mock = mock_stream_ptr.get();

    bool message_received = false;
    std::string received_payload;

    auto handler = [&](boost::uuids::uuid id, std::string msg) {
        message_received = true;
        received_payload = msg;
    };

    auto session = std::make_shared<GenericSession>(
        ioc,
        std::move(mock_stream_ptr),
        handler
    );

    SECTION("Start sequence performs handshake") {
        session->start();
        ioc.poll();

        REQUIRE(mock->handshake_cb != nullptr);

        // Simulate successful handshake
        mock->handshake_cb({});
        ioc.poll();

        // Should immediately try to read header
        REQUIRE(mock->read_cb != nullptr);
        REQUIRE(mock->pending_read_buf.size() == 20); // UUID(16) + Len(4)
    }

    SECTION("Send Message formats data correctly (Protocol Check)") {
        // Setup session state
        session->start();
        ioc.poll();
        mock->handshake_cb({});
        ioc.restart();
        ioc.poll();

        std::string test_msg = "Hello World";
        std::string req_id_str = generate_uuid_bytes(DEFAULT_UUID_SIZE);

        session->async_send_message(req_id_str, test_msg);
        ioc.restart();
        ioc.poll();

        // Verify write was triggered
        REQUIRE(mock->write_cb != nullptr);

        // Verify packet structure: UUID (16) + Len (4) + Payload (11)
        REQUIRE(mock->pending_write_buf.size() == DEFAULT_UUID_SIZE + 4 + test_msg.size());

        // Verify Length Header (Network Byte Order)
        const char *data_ptr = static_cast<const char *>(mock->pending_write_buf.data());

        uint32_t net_len;
        memcpy(&net_len, data_ptr + 16, 4);
        uint32_t host_len = ntohl(net_len);

        REQUIRE(host_len == test_msg.size());

        std::string sent_payload(data_ptr + 20, test_msg.size());
        REQUIRE(sent_payload == test_msg);
    }

    SECTION("Receive Message flow") {
        session->start();
        ioc.poll();
        mock->handshake_cb({});
        ioc.poll();

        REQUIRE(mock->read_cb != nullptr);
        REQUIRE(mock->pending_read_buf.size() == 20);

        boost::uuids::uuid u = boost::uuids::random_generator()();
        uint32_t len = htonl(5); // Payload is "Hello"

        char *buf_ptr = static_cast<char *>(mock->pending_read_buf.data());
        memcpy(buf_ptr, u.data, 16);
        memcpy(buf_ptr + 16, &len, 4);

        auto saved_read_cb = mock->read_cb; // Save cb because calling it might overwrite mock->read_cb
        saved_read_cb({}, 20);
        ioc.poll();

        REQUIRE(mock->pending_read_buf.size() == 5);

        buf_ptr = static_cast<char *>(mock->pending_read_buf.data());
        memcpy(buf_ptr, "Hello", 5);

        mock->read_cb({}, 5);
        ioc.poll();

        REQUIRE(message_received == true);
        REQUIRE(received_payload == "Hello");

        REQUIRE(mock->pending_read_buf.size() == 20);
    }

    SECTION("Oversized message triggers stop") {
        session->start();
        ioc.poll();
        mock->handshake_cb({});
        ioc.restart();
        ioc.poll();

        // malicious header (11MB)
        uint32_t len = htonl(11 * 1024 * 1024);
        char *buf_ptr = static_cast<char *>(mock->pending_read_buf.data());
        memcpy(buf_ptr + 16, &len, 4);

        mock->read_cb({}, 20);
        ioc.restart();
        ioc.poll();

        // Session should close because of the error
        REQUIRE(mock->close_called == true);
    }
}

TEST_CASE("GenericSession E2E: Session to Session", "[session][integration]") {
    boost::asio::io_context ioc;

    boost::asio::ip::tcp::acceptor acceptor(ioc, {boost::asio::ip::tcp::v4(), 0});
    boost::asio::ip::tcp::socket server_sock(ioc);
    boost::asio::ip::tcp::socket client_sock(ioc);

    // Connect the two sockets
    acceptor.async_accept(server_sock, [&](boost::system::error_code ec) {
        REQUIRE(!ec);
    });
    client_sock.async_connect(acceptor.local_endpoint(), [&](boost::system::error_code ec) {
        REQUIRE(!ec);
    });

    // need to run the loop quickly to establish TCP connection
    ioc.run_for(std::chrono::milliseconds(50));
    ioc.restart();

    REQUIRE(server_sock.is_open());
    REQUIRE(client_sock.is_open());

    // server setup
    std::string server_received_msg;
    bool server_got_msg = false;

    auto server_session = make_tcp_session(ioc, std::move(server_sock),
                                           [&](boost::uuids::uuid id, std::string msg) {
                                               server_received_msg = msg;
                                               server_got_msg = true;
                                           }
    );

    // client setup
    std::string client_received_msg;
    bool client_got_msg = false;

    auto client_session = make_tcp_session(ioc, std::move(client_sock),
                                           [&](boost::uuids::uuid id, std::string msg) {
                                               client_received_msg = msg;
                                               client_got_msg = true;
                                           }
    );

    // start both so they establish handshakes
    server_session->start();
    client_session->start();

    // Test client can send data to server
    std::string msg1 = "Hello Server";
    std::string uuid1 = make_dummy_uuid_str();

    client_session->async_send_message(uuid1, msg1);

    // give time for: Send -> Network -> Receive -> Parse -> Callback
    ioc.run_for(std::chrono::milliseconds(50));
    ioc.restart();

    REQUIRE(server_got_msg == true);
    REQUIRE(server_received_msg == "Hello Server");

    // Test server can send data to client
    std::string msg2 = "Hello Client, I got your message";
    std::string uuid2 = make_dummy_uuid_str();

    server_session->async_send_message(uuid2, msg2);

    // give time for the reply to travel back
    ioc.run_for(std::chrono::milliseconds(50));
    ioc.restart();

    REQUIRE(client_got_msg == true);
    REQUIRE(client_received_msg == "Hello Client, I got your message");

    client_session->stop();
    server_session->stop();
}
