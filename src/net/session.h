#pragma once
#include <memory>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/uuid/uuid.hpp>

#include "messages.pb.h"

using ReadMessageHandler = std::function<void(const boost::uuids::uuid &, const std::string &)>;

struct ISession : std::enable_shared_from_this<ISession> {
    virtual ~ISession() = default;

    virtual boost::asio::ip::basic_endpoint<boost::asio::ip::tcp> start() = 0;

    virtual void stop() = 0;

    virtual void async_send_message(const std::string &req_id, const std::string &message) = 0;

    virtual std::optional<std::string> remote_addr() const = 0;
};

std::shared_ptr<ISession> make_plain_session(
    boost::asio::io_context &ioc,
    boost::asio::ip::tcp::socket sock,
    ReadMessageHandler handler
);
