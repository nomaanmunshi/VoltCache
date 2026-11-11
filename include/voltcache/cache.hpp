#pragma once

#include <chrono>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace voltcache {

// Owned by the server's event loop; callers do not share this object across threads.
class Cache {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    struct Stats {
        std::uint64_t hits = 0, misses = 0, evictions = 0, expired = 0;
    };

    explicit Cache(std::size_t capacity);
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    Cache(Cache&&) = delete;
    Cache& operator=(Cache&&) = delete;
    void set(std::string key, std::string value, Time now = Clock::now());
    std::optional<std::string> get(const std::string& key, Time now = Clock::now());
    bool erase(const std::string& key, Time now = Clock::now());
    bool exists(const std::string& key, Time now = Clock::now());
    bool expire(const std::string& key, std::chrono::seconds ttl, Time now = Clock::now());
    std::int64_t ttl(const std::string& key, Time now = Clock::now());
    std::size_t cleanup(Time now = Clock::now());
    std::size_t size() const { return entries_.size(); }
    std::size_t capacity() const { return capacity_; }
    const Stats& stats() const { return stats_; }

private:
    struct Entry {
        std::string value;
        std::optional<Time> expires;
        std::list<std::string>::iterator recency;
    };
    using Map = std::unordered_map<std::string, Entry>;
    Map::iterator find_live(const std::string& key, Time now);
    void remove(Map::iterator it);
    std::size_t capacity_;
    std::list<std::string> recency_; // Most recently used at the front.
    Map entries_;
    Stats stats_;
};

} // namespace voltcache
