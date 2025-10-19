#include "session.h"

#include <iostream>
#include <boost/asio.hpp>

#include "RpcLayer.h"


using namespace boost::asio;

static void write_frame(ip::tcp::socket &sock, const std::string &payload) {
    if (payload.size() > std::numeric_limits<size_t>::max()) {
        throw std::length_error("payload too large");
    }

    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::vector<const_buffer> bufs;
    bufs.emplace_back(buffer(&len, sizeof(len)));
    bufs.push_back(buffer(payload));
    boost::asio::write(sock, bufs);
}

class PlainSession : public ISession {
public:
    PlainSession(io_context &ioc, ip::tcp::socket sock)
        : socket_(std::move(sock)), ioc_(ioc) {
    }

    void start() override {
        do_read_len();
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

    void async_send_message(const mesh::Envelope &message) override {
        std::string serialized_envelope = message.SerializeAsString();
        post(ioc_, [this, serialized_envelope]() {
            try {
                write_frame(socket_, serialized_envelope);
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

    void do_read_len() {
        auto self = shared_from_this();
        auto buf = std::make_shared<std::array<char, 4> >();
        async_read(socket_, buffer(*buf), [this, self, buf](boost::system::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "failed to read len message: " << ec.message() << std::endl;
                return;
            }
            uint32_t n;
            std::memcpy(&n, buf->data(), 4);
            n = ntohl(n);
            do_read_payload(n);
        });
    }

    void do_read_payload(uint32_t n) {
        auto self = shared_from_this();
        auto payload = std::make_shared<std::vector<char> >(n);
        async_read(socket_, buffer(*payload), [this, self, payload](boost::system::error_code ec, std::size_t) {
            if (ec) {
                std::cerr << "failed to read payload message: " << ec.message() << std::endl;
                return;
            }

            std::cout << "received message: \n" << payload->data() << std::endl;
        });
    }
};

std::shared_ptr<ISession> make_plain_session(io_context &ioc, ip::tcp::socket sock,
                                             std::function<void(std::shared_ptr<ISession> session)> callback) {
    return std::make_shared<PlainSession>(ioc, std::move(sock));
}
