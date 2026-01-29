#pragma once

#include <deque>
#include <unordered_set>
#include <tuple>
#include <utility>
#include <functional>

inline std::size_t hash_combine(std::size_t lhs, std::size_t rhs) {
    return lhs ^ (rhs + 0x9e3779b9 + (lhs << 6) + (lhs >> 2));
}

struct TupleHash {
    template<typename... Args>
    std::size_t operator()(const std::tuple<Args...> &t) const {
        return std::apply([](const auto &... args) {
            std::size_t seed = 0;
            ((seed = hash_combine(seed, std::hash<std::decay_t<decltype(args)> >{}(args))), ...);
            return seed;
        }, t);
    }
};

template<typename T, typename Hash = std::hash<T> >
class TimedDedupSet {
public:
    explicit TimedDedupSet(std::chrono::milliseconds max_age = std::chrono::seconds(30))
        : max_age_(max_age) {
    }

    bool insert(const T &value) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (set_.find(value) != set_.end()) {
            return false;
        }

        set_.insert(value);
        auto now = std::chrono::steady_clock::now();
        time_queue_.push_back({now, value});

        return true;
    }

    bool contains(const T &value) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_.find(value) != set_.end();
    }

    void cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (time_queue_.empty()) return;

        auto now = std::chrono::steady_clock::now();

        while (!time_queue_.empty()) {
            const auto &[timestamp, value] = time_queue_.front();

            if (now - timestamp > max_age_) {
                set_.erase(value);
                time_queue_.pop_front();
            } else {
                break;
            }
        }
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        set_.clear();
        time_queue_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::chrono::milliseconds max_age_;
    std::unordered_set<T, Hash> set_;
    std::deque<std::pair<std::chrono::steady_clock::time_point, T> > time_queue_;
};
