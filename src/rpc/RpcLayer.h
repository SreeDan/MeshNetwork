#pragma once
#include <functional>
#include <future>
#include <string>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include "session.h"

using namespace std::chrono_literals;

struct Pending {
    std::promise<std::optional<std::string> > prom;
    std::unique_ptr<boost::asio::steady_timer> timer;
};

class RpcConnection : std::enable_shared_from_this<RpcConnection> {
public:
    RpcConnection(boost::asio::io_context &ioc, boost::asio::ip::tcp::socket sock);

    ~RpcConnection();

    RpcConnection(RpcConnection &&other) noexcept;

    RpcConnection &operator=(RpcConnection &&other) noexcept;

    RpcConnection(const RpcConnection &other) = delete;

    RpcConnection &operator=(const RpcConnection &other) = delete;

    std::future<std::optional<std::string> > send_request(
        const std::string &wrapped_req,
        std::chrono::milliseconds timeout = 3000ms
    );

    std::future<std::optional<std::string> > send_request(
        const mesh::Envelope &envelope,
        std::chrono::milliseconds timeout = 3000ms
    );

    void store_response(const boost::uuids::uuid &msg_id, const std::string &resp_payload);

private:
    boost::asio::io_context &ioc_;
    // Guards pending_requests_
    std::mutex mu_;
    std::unordered_map<std::string, Pending> pending_requests_;
    std::shared_ptr<ISession> session_;
};

