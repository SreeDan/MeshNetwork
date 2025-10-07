#pragma once
#include <memory>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>

struct ISession : std::enable_shared_from_this<ISession> {
    virtual ~ISession() {}
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void async_send_message(const std::string& str) = 0;
    virtual std::string remote_addr() const = 0;
};

std::shared_ptr<ISession> make_plain_session(
    boost::asio::io_context& ioc,
    boost::asio::ip::tcp::socket sock,
    std::function<void(std::shared_ptr<ISession> session)> callback
);