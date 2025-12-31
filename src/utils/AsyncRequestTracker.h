#pragma once

#include <boost/asio.hpp>

// T is the type of any response payload (e.g., std::string or mesh::Envelope)
template<typename T>
class AsyncRequestTracker {
public:
    AsyncRequestTracker(boost::asio::io_context &ioc) : ioc_(ioc) {
    }

    std::future<T> track_request(const std::string &req_id, std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lock(mu_);

        std::promise<T> prom;
        auto fut = prom.get_future();

        auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, timeout);

        timer->async_wait([this, req_id](const boost::system::error_code &ec) {
            if (ec == boost::asio::error::operation_aborted) return;
            fail_request(req_id, std::make_exception_ptr(std::runtime_error("timeout")));
        });

        pending_requests_[req_id] = PendingRequest{std::move(prom), timer};
        return fut;
    }

    bool fulfill_request(const std::string &req_id, T response) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_requests_.find(req_id);
        if (it == pending_requests_.end()) return false;

        it->second.promise.set_value(std::move(response));
        it->second.timer->cancel();
        pending_requests_.erase(it);
        return true;
    }

private:
    struct PendingRequest {
        std::promise<T> promise;
        std::shared_ptr<boost::asio::steady_timer> timer;
    };

    boost::asio::io_context &ioc_;
    std::mutex mu_;
    std::unordered_map<std::string, PendingRequest> pending_requests_;

    void fail_request(const std::string &req_id, std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_requests_.find(req_id);
        if (it == pending_requests_.end()) return;

        try {
            it->second.promise.set_exception(e);
        } catch (...) {
        }

        pending_requests_.erase(it);
    }
};
