#pragma once

#include <iostream>
#include <boost/asio.hpp>

// T is the type of any response payload (e.g., std::string or mesh::Envelope)
template<typename T>
class AsyncRequestTracker {
public:
    using RequestCallback = std::function<void(const std::string &from_id, std::optional<std::string> payload)>;

    AsyncRequestTracker(boost::asio::io_context &ioc) : ioc_(ioc) {
    }

    void track_broadcast(const std::string &req_id,
                         std::chrono::milliseconds duration,
                         const std::function<void(const std::string &, T)> on_response,
                         const std::function<void()> &on_complete) {
        std::lock_guard<std::mutex> lock(mu_);

        auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, duration);
        // When timer expires, clean up and notify completion
        timer->async_wait([this, req_id, on_complete](const boost::system::error_code &ec) {
            // Note: We don't check operation_aborted here usually, because
            // for broadcasts, "timeout" IS the normal completion signal.

            std::lock_guard<std::mutex> lock(mu_);
            auto it = broadcasts_.find(req_id);
            if (it != broadcasts_.end()) {
                if (on_complete) on_complete(); // Notify caller we are done
                broadcasts_.erase(it);
            }
        });

        broadcasts_.try_emplace(
            req_id,
            BroadcastEntry{
                std::move(on_response),
                std::move(timer)
            }
        );
    }

    void track_request_callback(const std::string &req_id,
                                std::chrono::milliseconds timeout,
                                RequestCallback callback) {
        std::lock_guard<std::mutex> lock(mu_);

        auto timer = std::make_shared<boost::asio::steady_timer>(ioc_, timeout);

        timer->async_wait([this, req_id](const boost::system::error_code &ec) {
            if (ec == boost::asio::error::operation_aborted) return;
            complete_request(req_id, "", std::nullopt);
        });

        // Store the callback
        pending_requests_[req_id] = PendingRequest{std::move(callback), timer};
    }

    std::future<T> track_request(const std::string &req_id, std::chrono::milliseconds timeout) {
        std::lock_guard<std::mutex> lock(mu_);

        std::promise<T> prom;
        auto fut = prom.get_future();
        auto bridge_callback = [prom](const std::string &from, std::optional<T> payload) {
            if (payload.has_value()) {
                prom->set_value(std::move(payload.value()));
            } else {
                prom->set_exception(std::make_exception_ptr(std::runtime_error("timeout")));
            }
        };

        track_request_callback(req_id, timeout, bridge_callback);
        return fut;
    }

    bool fulfill_request(const std::string &req_id, const std::string &from_id, T payload) {
        if (complete_request(req_id, from_id, std::move(payload))) {
            return true;
        }

        // Handle if its a broadcast
        std::lock_guard<std::mutex> lock(mu_);
        auto bit = broadcasts_.find(req_id);
        if (bit != broadcasts_.end()) {
            bit->second.callback(from_id, std::move(payload));
            return true;
        }

        return false;
    }

private:
    struct PendingRequest {
        RequestCallback callback;
        std::shared_ptr<boost::asio::steady_timer> timer;
    };

    struct BroadcastEntry {
        std::function<void(const std::string &, T)> callback;
        std::shared_ptr<boost::asio::steady_timer> timer;
    };

    boost::asio::io_context &ioc_;
    std::mutex mu_;
    std::unordered_map<std::string, PendingRequest> pending_requests_;
    std::unordered_map<std::string, BroadcastEntry> broadcasts_;

    bool complete_request(const std::string &req_id, const std::string &from_id, std::optional<T> payload) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_requests_.find(req_id);
        if (it != pending_requests_.end()) {
            it->second.timer->cancel();

            it->second.callback(from_id, std::move(payload));
            pending_requests_.erase(it);
            return true;
        }
        return false;
    }

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
