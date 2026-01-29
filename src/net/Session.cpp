#include <iostream>
#include <boost/asio.hpp>
#include <boost/uuid/uuid.hpp>
#include <optional>

#include "AsyncWriter.h"
#include "Session.h"

#include "IStream.h"
#include "Stream.h"

using namespace boost::asio;


GenericSession::GenericSession(boost::asio::io_context &ioc, std::unique_ptr<StreamLayer> stream,
                               ReadMessageHandler handler)
    : ioc_(ioc),
      strand_(make_strand(ioc)),
      stream_(std::move(stream)),
      response_handler_(std::move(handler)) {
}

GenericSession::~GenericSession() {
    if (stream_) {
        stream_->close();
    }
}

void GenericSession::start() {
    auto self = shared_from_this();
    post(strand_, [this, self]() {
        stream_->async_handshake([this, self](boost::system::error_code ec) {
            if (ec) {
                std::cerr << "session: handshake failed: " << ec.message() << std::endl;
                stop();
                return;
            }
            do_read_header();
        });
    });
}

void GenericSession::stop() {
    post(strand_, [this, self = shared_from_this()]() {
        if (!stream_ || !stream_->is_open()) return;

        stream_->cancel();
        stream_->close();
        outbox_.clear();
    });
}

void GenericSession::async_send_message(const std::string &req_id, const std::string &message) {
    if (message.size() > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "payload too large, size: " << message.size() << std::endl;
        return;
    }

    auto data = std::make_shared<std::vector<char> >();
    data->reserve(16 + 4 * message.size());

    const auto &uuid = *reinterpret_cast<const boost::uuids::uuid *>(req_id.data());
    data->insert(data->end(), uuid.begin(), uuid.end());
    uint32_t payload_len = htonl(static_cast<uint32_t>(message.size()));
    const char *len_ptr = reinterpret_cast<const char *>(&payload_len);

    data->insert(data->end(), len_ptr, len_ptr + sizeof(payload_len));
    data->insert(data->end(), message.begin(), message.end());

    // post to strand and not ioc_
    // this serializes the access to the outbox in case multiple threads push at once
    post(strand_, [this, data, self = shared_from_this()]() {
        bool write_in_progress = !outbox_.empty();
        outbox_.push_back(data);
        if (!write_in_progress) {
            do_write();
        }
    });
}

std::optional<std::string> GenericSession::remote_addr() const {
    if (!stream_)
        return std::nullopt;
    return stream_->remote_address();
}


void GenericSession::do_write() {
    if (outbox_.empty()) return;

    auto &msg = outbox_.front();
    auto self = shared_from_this();
    stream_->async_write_fully(buffer(*msg),
                               bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
                                   if (ec) {
                                       stop();
                                       return;
                                   }
                                   outbox_.pop_front();
                                   if (!outbox_.empty()) {
                                       do_write();
                                   }
                               }));
}

void GenericSession::do_read_header() {
    // Buffer for the header (UUID + Len)
    auto self = shared_from_this();
    auto header_buf = std::make_shared<std::array<char, 20> >();
    stream_->async_read_fully(buffer(*header_buf),
                              bind_executor(strand_,
                                            [this, self, header_buf](boost::system::error_code ec, std::size_t) {
                                                if (ec) {
                                                    if (ec != boost::asio::error::eof)
                                                        std::cerr << "[session] Read header failed: " << ec.
                                                                message() <<
                                                                std::endl;
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
                                                    std::cerr << "[session] Oversized message: " << len <<
                                                            std::endl;
                                                    stop();
                                                    return;
                                                }

                                                do_read_payload(msg_id, len);
                                            })
    );
}


void GenericSession::do_read_payload(boost::uuids::uuid msg_id, uint32_t len) {
    auto self = shared_from_this();
    auto payload = std::make_shared<std::vector<char> >(len);
    stream_->async_read_fully(buffer(*payload),
                              bind_executor(strand_,
                                            [this ,self, payload, msg_id
                                            ](boost::system::error_code ec, std::size_t) {
                                                if (ec) {
                                                    std::cerr << "[session] Read payload failed: " << ec.message()
                                                            << std::endl;
                                                    stop();
                                                    return;
                                                }

                                                response_handler_(
                                                    msg_id, std::string(payload->data(), payload->size()));

                                                // read the next message
                                                do_read_header();
                                            })
    );
}

std::shared_ptr<ISession> make_tcp_session(io_context &ioc, ip::tcp::socket sock, ReadMessageHandler handler) {
    auto stream = std::make_unique<TcpStream>(std::move(sock));
    return std::make_shared<GenericSession>(ioc, std::move(stream), handler);
}

std::shared_ptr<ISession> make_ssl_session(boost::asio::io_context &ioc, boost::asio::ip::tcp::socket sock,
                                           boost::asio::ssl::context &ctx, bool is_server, ReadMessageHandler handler) {
    auto stream = std::make_unique<SslStream>(std::move(sock), ctx, is_server);
    return std::make_shared<GenericSession>(ioc, std::move(stream), handler);
}

