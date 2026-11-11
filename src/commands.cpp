#include "voltcache/commands.hpp"
#include <stdexcept>

namespace voltcache {
Message execute(Cache& cache, Message request, Cache::Time now) {
    if (request.empty()) return {"ERROR", "COMMAND", "empty command"};
    auto& command = request[0];
    for (auto& c : command) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (command == "PING" && request.size() == 1) return {"PONG"};
    if (command == "STATS" && request.size() == 1) {
        cache.cleanup(now);
        const auto& s = cache.stats();
        return {"STATS", "entries", std::to_string(cache.size()),
                "capacity", std::to_string(cache.capacity()),
                "hits", std::to_string(s.hits), "misses", std::to_string(s.misses),
                "evictions", std::to_string(s.evictions), "expired_entries_removed", std::to_string(s.expired)};
    }
    const bool known = command == "SET" || command == "GET" || command == "DEL" || command == "EXISTS" ||
                       command == "EXPIRE" || command == "TTL";
    if (!known) return {"ERROR", "COMMAND", "unknown command"};
    const std::size_t expected = command == "SET" || command == "EXPIRE" ? 3 : 2;
    if (request.size() != expected && !(command == "SET" && request.size() == 5)) return {"ERROR", "ARITY", "wrong number of arguments"};
    const auto& key = request[1];
    if (key.empty() || key.size() > max_key) return {"ERROR", "KEY", "key must contain 1 to 256 bytes"};
    if (command == "SET") {
        if (request[2].size() > max_value) return {"ERROR", "VALUE", "value exceeds 60000 bytes"};
        std::optional<std::uint64_t> ttl;
        if (request.size() == 5) {
            if (request[3] != "EX" && request[3] != "ex") return {"ERROR", "SYNTAX", "use SET key value EX seconds"};
            try { ttl = number(request[4], 1, max_ttl); }
            catch (const std::invalid_argument&) { return {"ERROR", "TTL", "SET EX requires 1 to 31536000 seconds"}; }
        }
        cache.set(key, std::move(request[2]), now);
        if (ttl) cache.expire(key, std::chrono::seconds(*ttl), now);
        return {"OK"};
    }
    if (command == "GET") {
        auto value = cache.get(key, now);
        return value ? Message{"VALUE", std::move(*value)} : Message{"NOT_FOUND"};
    }
    if (command == "DEL") return {"INT", cache.erase(key, now) ? "1" : "0"};
    if (command == "EXISTS") return {"INT", cache.exists(key, now) ? "1" : "0"};
    if (command == "TTL") return {"INT", std::to_string(cache.ttl(key, now))};
    try {
        auto seconds = number(request[2], 0, max_ttl);
        return {"INT", cache.expire(key, std::chrono::seconds(seconds), now) ? "1" : "0"};
    } catch (const std::invalid_argument&) {
        return {"ERROR", "TTL", "TTL must be an integer from 0 to 31536000 seconds"};
    }
}
} // namespace voltcache
