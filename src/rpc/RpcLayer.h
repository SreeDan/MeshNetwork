#pragma once
#include <functional>
#include <future>
#include <string>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

using namespace std::chrono_literals;

struct Pending {
    std::promise<std::string> prom;
    std::unique_ptr<boost::asio::steady_timer> timer;
};

class RpcLayer {
public:
    using SendFn = std::function<void(const std::string &peer_addr, const std::string &payload)>;

    RpcLayer(boost::asio::io_context &ioc, SendFn sender);

    ~RpcLayer();

    RpcLayer(RpcLayer &&other) noexcept;

    RpcLayer &operator=(RpcLayer &&other) noexcept;

    RpcLayer(const RpcLayer &other) = delete;

    RpcLayer &operator=(const RpcLayer &other) = delete;

    std::future<std::string> send_request(
        const std::string &peer_addr,
        const std::string &wrapped_req,
        std::chrono::milliseconds timeout = 3000ms
    );

    void receive_response(const std::string &req_id, const std::string &resp_payload);

private:
    boost::asio::io_context &ioc_;
    SendFn send_fn_;
    std::mutex mu_;
    std::unordered_map<std::string, Pending> pending_requests_;
};

