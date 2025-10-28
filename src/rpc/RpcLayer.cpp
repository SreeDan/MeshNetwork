#include "RpcLayer.h"

#include <iostream>
#include <utility>
#include <boost/asio/steady_timer.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>


using namespace std::chrono_literals;

RpcConnection::RpcConnection(boost::asio::io_context &ioc, boost::asio::ip::tcp::socket sock)
    : ioc_(ioc) {
    auto self = shared_from_this();
    session_ = make_plain_session(ioc, std::move(sock),
                                  [self](const boost::uuids::uuid &msg_id, const std::string &payload) {
                                      self->store_response(msg_id, payload);
                                  });
    // TODO: Eventually handle constructing a TLS session
}

RpcConnection::~RpcConnection() {
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

RpcConnection::RpcConnection(RpcConnection &&other) noexcept
    : ioc_(other.ioc_),
      pending_requests_(std::move(other.pending_requests_)) {
}

RpcConnection &RpcConnection::operator=(RpcConnection &&other) noexcept {
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
        pending_requests_ = std::move(other.pending_requests_);
    }

    return *this;
}

std::future<std::optional<std::string> > RpcConnection::send_request(
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

    std::promise<std::optional<std::string> > promise;
    auto fut = promise.get_future();

    auto timer = std::make_unique<boost::asio::steady_timer>(ioc_, timeout);
    auto time_ptr = timer.get();
    {
        std::lock_guard<std::mutex> guard(mu_);
        pending_requests_[req_id] = Pending{
            std::move(promise),
            std::move(timer)
        };
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

    // TODO: Make a whitelist of types where we know if it will capture a response or not
    session_->async_send_message(req_id, payload);

    return fut;
}

std::future<std::optional<std::string> > RpcConnection::send_request(
    const mesh::Envelope &envelope,
    std::chrono::milliseconds timeout) {
    return send_request(envelope.SerializeAsString(), timeout);
}

void RpcConnection::store_response(const boost::uuids::uuid &msg_id, const std::string &response) {
    std::lock_guard<std::mutex> guard(mu_);
    std::string str_id = boost::uuids::to_string(msg_id);
    if (pending_requests_.contains(str_id)) {
        auto it = pending_requests_.find(str_id);
        try {
            it->second.prom.set_value(response);
        } catch (...) {
            std::cerr << "receive response failed for request " << str_id << std::endl;
        }
        pending_requests_.erase(it);
    }
}
