#pragma once
#include <cstddef>
#include <deque>
#include <optional>
#include <stdexcept>
#include <utility>

namespace voltcache {
// Like the cache, this queue is confined to one event-loop thread.
template <typename T> class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t limit) : limit_(limit) {
        if (!limit) throw std::invalid_argument("queue capacity must be positive");
    }
    bool push(T value) {
        if (items_.size() == limit_) return false;
        items_.push_back(std::move(value));
        if (items_.size() > peak_) peak_ = items_.size();
        return true;
    }
    std::optional<T> pop() {
        if (items_.empty()) return std::nullopt;
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }
    std::size_t size() const { return items_.size(); }
    std::size_t peak() const { return peak_; }
private:
    std::size_t limit_, peak_ = 0;
    std::deque<T> items_;
};
} // namespace voltcache
