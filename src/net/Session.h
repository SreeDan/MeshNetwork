#pragma once
#include <deque>
#include <memory>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/uuid/uuid.hpp>

#include "IStream.h"

using ReadMessageHandler = std::function<void(const boost::uuids::uuid &, const std::string &)>;

struct ISession : std::enable_shared_from_this<ISession> {
    virtual ~ISession() = default;

    virtual void start() = 0;

    virtual void stop() = 0;

    virtual void async_send_message(const std::string &req_id, const std::string &message) = 0;

    virtual std::optional<std::string> remote_addr() const = 0;
};

class GenericSession : public ISession {
public:
    GenericSession(boost::asio::io_context &ioc, std::unique_ptr<StreamLayer> stream, ReadMessageHandler handler);

    ~GenericSession() override;

    void start() override;

    void stop() override;

    void async_send_message(const std::string &req_id, const std::string &message) override;

    std::optional<std::string> remote_addr() const override;

private:
    boost::asio::io_context &ioc_;
    // strand is for sequential execution of things in the boost io thread
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::unique_ptr<StreamLayer> stream_;
    ReadMessageHandler response_handler_;

    std::deque<std::shared_ptr<std::vector<char> > > outbox_;

    void do_write();

    void do_read_header();

    void do_read_payload(boost::uuids::uuid msg_id, uint32_t len);
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
