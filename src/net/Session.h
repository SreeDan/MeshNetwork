#pragma once
#include <memory>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/uuid/uuid.hpp>

using ReadMessageHandler = std::function<void(const boost::uuids::uuid &, const std::string &)>;

struct ISession : std::enable_shared_from_this<ISession> {
    virtual ~ISession() = default;

    virtual void start() = 0;

    virtual void stop() = 0;

    virtual void async_send_message(const std::string &req_id, const std::string &message) = 0;

    virtual std::optional<std::string> remote_addr() const = 0;
};

std::shared_ptr<ISession> make_tcp_session(
    boost::asio::io_context &ioc,
    boost::asio::ip::tcp::socket sock,
    ReadMessageHandler handler
);

std::shared_ptr<ISession> make_ssl_session(
    boost::asio::io_context &ioc,
    boost::asio::ip::tcp::socket sock,
    boost::asio::ssl::context &ctx,
    bool is_server,
    ReadMessageHandler handler
);
