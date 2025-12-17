#include "RpcConnection.h"

#include <iostream>
#include <boost/asio.hpp>
#include <boost/uuid/uuid.hpp>
#include <optional>
#include <boost/uuid/uuid_io.hpp>

#include "async_writer.h"
#include "session.h"

using namespace boost::asio;

class PlainSession : public ISession {
public:
    PlainSession(io_context &ioc, ip::tcp::socket sock, ReadMessageHandler handler)
        : socket_(std::move(sock)),
          ioc_(ioc),
          response_handler_(std::move(handler)),
          write_queue_(std::make_shared<AsyncWriteQueue<ip::tcp::socket> >(socket_)) {
    }

    PlainSession(PlainSession &&other) noexcept = delete;

    PlainSession &operator=(PlainSession &&other) noexcept = delete;

    PlainSession(const PlainSession &) = delete;

    PlainSession &operator=(const PlainSession &) = delete;

    ~PlainSession() override {
        PlainSession::stop();
    }

    ip::basic_endpoint<ip::tcp> start() override {
        do_read_header();
        return socket_.remote_endpoint();
    }

    void stop() override {
        if (!socket_.is_open())
            return;
        if (write_queue_)
            write_queue_->cancel();

        boost::system::error_code ec;
        socket_.shutdown(ip::tcp::socket::shutdown_both, ec);
        if (ec) {
            std::cerr << "failed to shutdown socket " << ec.message() << std::endl;
        }

        socket_.close(ec);
        if (ec) {
            std::cerr << "failed to close socket " << ec.message() << std::endl;
        }
    }

    void async_send_message(const std::string &req_id, const std::string &message) override {
        if (message.size() > std::numeric_limits<uint32_t>::max()) {
            std::cerr << "payload too large" << std::endl;
            return;
        }

        auto data = std::make_shared<std::vector<char> >();
        data->reserve(16 + 4 * message.size());

        data->insert(data->end(), req_id.begin(), req_id.end());
        uint32_t payload_len = htonl(static_cast<uint32_t>(message.size()));
        const char *len_ptr = reinterpret_cast<const char *>(&payload_len);

        data->insert(data->end(), len_ptr, len_ptr + sizeof(payload_len));
        data->insert(data->end(), message.begin(), message.end());
        std::cout << "writing data to queue" << std::endl;
        write_queue_->write(std::move(data));
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
    std::shared_ptr<AsyncWriteQueue<ip::tcp::socket> > write_queue_;

    void do_read_header() {
        auto self = shared_from_this();
        // Buffer for the header (UUID + Len)
        auto header_buf = std::make_shared<std::array<char, 20> >();
        async_read(socket_, buffer(*header_buf),
                   [this, self, header_buf](boost::system::error_code ec, std::size_t) {
                       if (ec) {
                           if (ec != boost::asio::error::eof)
                               std::cerr << "[session] Read header failed: " << ec.message() << std::endl;
                           stop();
                           return;
                       }

                       boost::uuids::uuid msg_id{};
                       uint32_t len;

                       memcpy(msg_id.data, header_buf->data(), 16);
                       memcpy(&len, header_buf->data() + 16, 4);
                       len = ntohl(len);

                       if (len > 10 * 1024 * 1024) {
                           // 10MB limit
                           std::cerr << "[session] Oversized message: " << len << std::endl;
                           stop();
                           return;
                       }

                       do_read_payload(msg_id, len);
                   });
    }

    void do_read_payload(boost::uuids::uuid msg_id, uint32_t len) {
        auto self = shared_from_this();
        auto payload = std::make_shared<std::vector<char> >(len);
        async_read(socket_, buffer(*payload),
                   [this ,self, payload, msg_id](boost::system::error_code ec, std::size_t) {
                       if (ec) {
                           std::cerr << "[session] Read payload failed: " << ec.message() << std::endl;
                           stop();
                           return;
                       }

                       response_handler_(msg_id, std::string(payload->data(), payload->size()));

                       // read the next message
                       do_read_header();
                   });
    }
};

std::shared_ptr<ISession> make_plain_session(io_context &ioc, ip::tcp::socket sock, ReadMessageHandler handler) {
    return std::make_shared<PlainSession>(ioc, std::move(sock), handler);
}
