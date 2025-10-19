#include "RpcLayer.h"

#include <iostream>
#include <utility>
#include <boost/asio/steady_timer.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>


using namespace std::chrono_literals;

RpcLayer::RpcLayer(boost::asio::io_context &ioc, SendFn sender)
    : ioc_(ioc), send_fn_(std::move(sender)) {
}

RpcLayer::~RpcLayer() {
    std::lock_guard<std::mutex> guard(mu_);
    for (auto &[req_id, pending_req]: pending_requests_) {
        if (pending_req.timer) {
            boost::system::error_code ec;
            pending_req.timer->cancel(ec);
        }

        try {
            pending_req.prom.set_exception(
                std::make_exception_ptr(std::runtime_error("RpcLayer destroyed")));
        } catch (...) {
        }
    }
    pending_requests_.clear();
}

RpcLayer::RpcLayer(RpcLayer &&other) noexcept
    : ioc_(other.ioc_),
      send_fn_(std::move(other.send_fn_)),
      pending_requests_(std::move(other.pending_requests_)) {
}

RpcLayer &RpcLayer::operator=(RpcLayer &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    for (auto &[req_id, pending_req]: pending_requests_) {
        if (pending_req.timer) {
            boost::system::error_code ec;
            pending_req.timer->cancel(ec);
        }

        try {
            pending_req.prom.set_exception(
                std::make_exception_ptr(std::runtime_error("RpcLayer moved")));
        } catch (...) {
        }
    }

    {
        // Move from other
        std::lock_guard<std::mutex> g(other.mu_);
        send_fn_ = std::move(other.send_fn_);
        pending_requests_ = std::move(other.pending_requests_);
    }

    return *this;
}


std::future<std::string> RpcLayer::send_request(
    const std::string &peer_addr,
    const std::string &wrapped_req,
    std::chrono::milliseconds timeout) {
    static boost::uuids::random_generator generator;
    std::string req_id = boost::uuids::to_string(generator());

    // format is 2 bytes of the len(req_id) + req_id + wrapped_req
    uint16_t len = static_cast<uint16_t>(req_id.size());
    std::string payload;
    payload.append(reinterpret_cast<const char *>(&len), sizeof(len));
    payload.append(req_id);
    payload.append(wrapped_req);

    std::promise<std::string> promise;
    auto fut = promise.get_future();

    auto timer = std::make_unique<boost::asio::steady_timer>(ioc_, timeout);
    auto time_ptr = timer.get();
    {
        std::lock_guard<std::mutex> guard(mu_);
        pending_requests_[req_id] = Pending{std::move(promise), std::move(timer)};
    }

    // timer expiration logic
    time_ptr->async_wait([this, req_id](const boost::system::error_code &ec) {
        if (ec == boost::asio::error::operation_aborted) {
            // timer was cancelled, meaning the response did not time out
            return;
        }

        std::lock_guard<std::mutex> guard(mu_);
        auto it = pending_requests_.find(req_id);
        if (it == pending_requests_.end()) {
            return;
        }

        try {
            it->second.prom.set_exception(
                std::make_exception_ptr(std::runtime_error("RPC failed: timeout")));
        } catch (...) {
        }
        pending_requests_.erase(it);
    });


    try {
        send_fn_(peer_addr, payload);
    } catch (...) {
        receive_response(req_id, "");
    }

    return fut;
}

void RpcLayer::receive_response(const std::string &req_id, const std::string &response) {
    std::lock_guard<std::mutex> guard(mu_);
    if (pending_requests_.contains(req_id)) {
        auto it = pending_requests_.find(req_id);
        try {
            it->second.prom.set_value(response);
        } catch (...) {
            std::cerr << "receive response failed for request " << req_id << std::endl;
        }
        pending_requests_.erase(it);
    }
}

