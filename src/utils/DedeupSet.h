#pragma once

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
class DedupSet {
public:
    bool insert(const T &value) {
        auto result = set_.insert(value);
        return result.second; // true if it was new
    }

    bool contains(const T &value) const {
        return set_.find(value) != set_.end();
    }

    std::size_t size() const { return set_.size(); }

    void clear() { set_.clear(); }

private:
    std::unordered_set<T, Hash> set_;
};
