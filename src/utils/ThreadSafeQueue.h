#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

template<typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;

    ThreadSafeQueue(const ThreadSafeQueue &) = delete;

    ThreadSafeQueue &operator=(const ThreadSafeQueue &) = delete;

    void push(T value) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push(std::move(value));
        }
        cond_.notify_one();
    }

    // Blocking pop - waits indefinitely until an item is available
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mu_);
        cond_.wait(lock, [this] { return !queue_.empty() || stop_requested_; });

        if (stop_requested_ && queue_.empty()) {
            return std::nullopt;
        }

        T val = std::move(queue_.front());
        queue_.pop();
        return val;
    }

    // Timeout pop - waits for 'duration'. Returns std::nullopt if timed out.
    template<typename Rep, typename Period>
    std::optional<T> pop_for(const std::chrono::duration<Rep, Period> &duration) {
        std::unique_lock<std::mutex> lock(mu_);

        // Wait returns false if the timeout expired
        bool success = cond_.wait_for(lock, duration, [this] {
            return !queue_.empty() || stop_requested_;
        });

        if (stop_requested_ && queue_.empty()) {
            return std::nullopt;
        }

        if (!success) {
            return std::nullopt; // Timeout
        }

        T val = std::move(queue_.front());
        queue_.pop();
        return val;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mu_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return queue_.size();
    }

    // Unblocks any waiting threads so they can exit (e.g., during shutdown)
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_requested_ = true;
        }
        cond_.notify_all();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mu_;
    std::condition_variable cond_;
    bool stop_requested_ = false;
};
