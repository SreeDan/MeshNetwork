#include "session.h"

#include <iostream>
#include <boost/asio.hpp>
#include <boost/uuid/uuid.hpp>

#include "RpcLayer.h"


using namespace boost::asio;

static void write_frame(ip::tcp::socket &sock, const std::string &req_id, const std::string &payload) {
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::length_error("payload too large");
    }

    uint32_t payload_len = htonl(static_cast<uint32_t>(payload.size()));
    std::vector<const_buffer> bufs{
        buffer(req_id),
        buffer(&payload_len, sizeof(uint32_t)),
        buffer(payload)
    };

    boost::asio::write(sock, bufs);
}

class PlainSession : public ISession {
public:
    PlainSession(io_context &ioc, ip::tcp::socket sock, ReadMessageHandler handler)
        : socket_(std::move(sock)), ioc_(ioc), response_handler_(std::move(handler)) {
    }

    void start() override {
        do_read_metadata();
    }

    void stop() override {
        if (!socket_.is_open()) {
            return;
        }
        boost::system::error_code ec;
        socket_.shutdown(ip::tcp::socket::shutdown_both, ec);
        if (ec) {
            std::cerr << "failed to shutdown socket " << ec.message() << std::endl;
            return;
        }
        socket_.close(ec);
        if (ec) {
            std::cerr << "failed to close socket " << ec.message() << std::endl;
            return;
        }
    }

    void async_send_message(const std::string &req_id, const std::string &message) override {
        post(ioc_, [this, message, req_id]() {
            try {
                write_frame(socket_, req_id, message);
            } catch (std::exception &e) {
                std::cerr << "failed to send message: " << e.what() << std::endl;
            }
        });
    }

    std::optional<std::string> remote_addr() const override {
        try {
            return socket_.remote_endpoint().address().to_string();
        } catch (...) {
            return std::nullopt;
        }
    }

private:
    ip::tcp::socket socket_;
    io_context &ioc_;
    std::function<void(const boost::uuids::uuid &, const std::string &)> response_handler_;

    void do_read_metadata() {
        auto self = shared_from_this();
        auto buf = std::make_shared<std::array<char, 20> >();
        async_read(socket_, buffer(*buf), [this, self, buf](boost::system::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "failed to read len message: " << ec.message() << std::endl;
                return;
            }

            // format of message is [ 16 bytes UUID ][ 4 bytes length ][ N bytes payload ]

            boost::uuids::uuid msg_id;
            uint32_t len;

            std::memcpy(msg_id.data, buf->data(), 16);
            // Copy length (starts after 16 bytes)
            std::memcpy(&len, buf->data() + 16, 4);
            len = ntohl(len);

            do_read_payload(msg_id, len);
        });
    }

    void do_read_payload(boost::uuids::uuid msg_id, uint32_t n) {
        auto self = shared_from_this();
        auto payload = std::make_shared<std::vector<char> >(n);
        async_read(socket_, buffer(*payload), [this, self, payload, msg_id](boost::system::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "failed to read payload message: " << ec.message() << std::endl;
                return;
            }

            std::cout << "received message: \n" << payload->data() << std::endl;

            response_handler_(msg_id, payload->data());
        });
    }
};

std::shared_ptr<ISession> make_plain_session(io_context &ioc, ip::tcp::socket sock, ReadMessageHandler handler) {
    return std::make_shared<PlainSession>(ioc, std::move(sock), handler);
}
