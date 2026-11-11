#include "voltcache/cache.hpp"
#include <stdexcept>

namespace voltcache {

Cache::Cache(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) throw std::invalid_argument("capacity must be positive");
}

void Cache::remove(Map::iterator it) {
    recency_.erase(it->second.recency);
    entries_.erase(it);
}

Cache::Map::iterator Cache::find_live(const std::string& key, Time now) {
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.expires && now >= *it->second.expires) {
        remove(it);
        ++stats_.expired;
        return entries_.end();
    }
    return it;
}

void Cache::set(std::string key, std::string value, Time now) {
    auto it = find_live(key, now);
    if (it != entries_.end()) {
        it->second.value = std::move(value);
        it->second.expires.reset(); // SET replaces the value and clears its TTL.
        recency_.splice(recency_.begin(), recency_, it->second.recency);
        return;
    }
    if (entries_.size() >= capacity_) {
        // Reclaim expired entries before evicting a still-live one.
        cleanup(now);
        if (entries_.size() >= capacity_) {
            remove(entries_.find(recency_.back()));
            ++stats_.evictions;
        }
    }
    recency_.push_front(key);
    try {
        entries_.emplace(std::move(key), Entry{std::move(value), std::nullopt, recency_.begin()});
    } catch (...) {
        recency_.pop_front();
        throw;
    }
}

std::optional<std::string> Cache::get(const std::string& key, Time now) {
    auto it = find_live(key, now);
    if (it == entries_.end()) {
        ++stats_.misses;
        return std::nullopt;
    }
    ++stats_.hits;
    recency_.splice(recency_.begin(), recency_, it->second.recency);
    return it->second.value;
}

bool Cache::erase(const std::string& key, Time now) {
    auto it = find_live(key, now);
    if (it == entries_.end()) return false;
    remove(it);
    return true;
}

bool Cache::exists(const std::string& key, Time now) {
    return find_live(key, now) != entries_.end();
}

bool Cache::expire(const std::string& key, std::chrono::seconds ttl, Time now) {
    auto it = find_live(key, now);
    if (it == entries_.end()) return false;
    if (ttl.count() == 0) remove(it);
    else {
        if (ttl.count() < 0 || ttl > std::chrono::duration_cast<std::chrono::seconds>(Time::max() - now))
            throw std::invalid_argument("TTL out of range");
        it->second.expires = now + ttl;
    }
    return true;
}

std::int64_t Cache::ttl(const std::string& key, Time now) {
    auto it = find_live(key, now);
    if (it == entries_.end()) return -2;
    if (!it->second.expires) return -1;
    return std::chrono::duration_cast<std::chrono::seconds>(*it->second.expires - now).count();
}

std::size_t Cache::cleanup(Time now) {
    std::size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires && now >= *it->second.expires) {
            auto expired = it++;
            remove(expired);
            ++removed;
        } else ++it;
    }
    stats_.expired += removed;
    return removed;
}

} // namespace voltcache
