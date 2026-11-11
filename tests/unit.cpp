#include "voltcache/cache.hpp"
#include "voltcache/commands.hpp"
#include "voltcache/protocol.hpp"
#include "voltcache/queue.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace voltcache;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template <typename F> void rejects(F fn, const char* message) {
    bool caught = false;
    try { fn(); } catch (const std::invalid_argument&) { caught = true; }
    check(caught, message);
}
void policies() {
    const Cache::Time now{};
    using namespace std::chrono_literals;
    rejects([] { Cache invalid(0); }, "zero capacity");
    Cache cache(3);
    cache.set("a", "A", now);
    cache.set("b", "B", now);
    cache.set("c", "C", now);
    check(cache.get("a", now) == "A", "GET retrieves entry");
    cache.set("d", "D", now);
    check(!cache.exists("b", now), "GET changes LRU victim");
    check(cache.stats().evictions == 1, "eviction counted");
    check(cache.exists("c", now), "EXISTS sees live entry");
    cache.set("e", "E", now);
    check(!cache.exists("c", now), "EXISTS does not change recency");
    check(cache.expire("a", 2s, now), "EXPIRE existing");
    check(cache.ttl("a", now) == 2, "initial TTL");
    check(cache.ttl("a", now + 1500ms) == 0, "TTL rounds down");
    check(cache.get("a", now + 1999ms) == "A", "live immediately before expiry");
    check(!cache.get("a", now + 2s), "expired exactly on boundary");
    check(cache.ttl("a", now + 2s) == -2, "TTL missing");
    check(cache.stats().expired == 1, "lazy expiration counted");
    cache.expire("d", 1s, now);
    cache.set("d", "replacement", now);
    check(cache.ttl("d", now) == -1, "SET clears TTL");
    check(cache.get("d", now + 2s) == "replacement", "replacement survives old deadline");
    check(cache.expire("d", 0s, now), "EXPIRE zero removes immediately");
    check(!cache.erase("d", now), "DEL missing");
    check(!cache.expire("missing", 1s, now), "EXPIRE missing");
    rejects([&] { cache.expire("e", -1s, now); }, "negative duration rejected");
    Cache reclaim(2);
    reclaim.set("live", "L", now);
    reclaim.set("stale", "S", now);
    reclaim.expire("stale", 1s, now);
    reclaim.set("new", "N", now + 1s);
    check(reclaim.exists("live", now + 1s), "expired entry reclaimed before live LRU");
    check(reclaim.stats().evictions == 0 && reclaim.stats().expired == 1, "reclaim statistics");
    reclaim.expire("new", 1s, now + 1s);
    check(reclaim.cleanup(now + 2s) == 1, "periodic cleanup");
    check(reclaim.size() == 1, "cleanup size");
    Cache updates(2);
    updates.set("a", "1", now);
    updates.set("b", "2", now);
    updates.set("a", "3", now);
    updates.set("c", "4", now);
    check(!updates.exists("b", now) && updates.get("a", now) == "3", "SET refreshes recency");
}
void protocol() {
    const Message values{"SET", std::string("k\0x", 3), std::string("hello\n\0world", 12)};
    auto bytes = frame(values);
    check(read_u32(reinterpret_cast<const unsigned char*>(bytes.data())) == bytes.size() - 4, "network length");
    check(decode(std::string_view(bytes).substr(4)) == values, "binary roundtrip");
    check(parse_command("SET name \"Nomaan Munshi\"") == Message({"SET", "name", "Nomaan Munshi"}), "quoted CLI value");
    check(parse_command("SET x ''") == Message({"SET", "x", ""}), "empty CLI value");
    check(parse_command("SET x 'a\\nb'")[2] == "a\nb", "CLI escaped newline");
    check(parse_command("  \t").empty(), "blank command");
    rejects([] { parse_command("SET x 'unfinished"); }, "unclosed quote");
    rejects([] { parse_command("SET x \\"); }, "unfinished escape");
    rejects([] { decode(""); }, "empty payload");
    rejects([] { decode(std::string("\0\0", 2)); }, "zero fields");
    rejects([] { decode(std::string("\0\1\0", 3)); }, "short field length");
    rejects([] { decode(std::string("\0\1\xff\xff\xff\xff", 6)); }, "oversized field length");
    rejects([&] { decode(std::string_view(bytes).substr(4, bytes.size() - 5)); }, "truncated payload");
    rejects([&] { decode(bytes.substr(4) + "x"); }, "trailing payload");
    rejects([] { frame({"SET", "x", std::string(max_frame, 'x')}); }, "outgoing limit");
    rejects([] { number("-1", 0, 10); }, "negative integer");
    rejects([] { number("12x", 0, 100); }, "integer suffix");
    rejects([] { number("18446744073709551616", 0, 100); }, "integer overflow");
}
void commands() {
    Cache cache(4);
    const Cache::Time now{};
    check(execute(cache, {"ping"}, now) == Message{"PONG"}, "case insensitive command");
    check(execute(cache, {"SET", "key", ""}, now) == Message{"OK"}, "empty value allowed");
    check(execute(cache, {"GET", "key"}, now) == Message({"VALUE", ""}), "empty value distinct from missing");
    check(execute(cache, {"SET", "key", "new", "EX", "2"}, now) == Message{"OK"}, "atomic SET EX");
    check(execute(cache, {"GET", "key"}, now + std::chrono::seconds(2)) == Message{"NOT_FOUND"}, "SET EX expires");
    check(execute(cache, {"EXPIRE", "key", "-1"}, now)[1] == "TTL", "negative protocol TTL");
    check(execute(cache, {"SET", "key", "new", "EX", "0"}, now)[1] == "TTL", "zero SET EX rejected");
    check(!cache.exists("key", now), "invalid command cannot mutate key");
    check(execute(cache, {"GET"}, now)[1] == "ARITY", "wrong arity");
    check(execute(cache, {"unknown"}, now)[1] == "COMMAND", "unknown command");
    check(execute(cache, {"SET", "", "x"}, now)[1] == "KEY", "empty key rejected");
    check(execute(cache, {"SET", std::string(257, 'k'), "x"}, now)[1] == "KEY", "key limit");
    check(execute(cache, {"SET", "k", std::string(60001, 'x')}, now)[1] == "VALUE", "value limit");
    execute(cache, {"SET", "key", "value"}, now);
    auto hits = cache.stats().hits;
    execute(cache, {"EXISTS", "key"}, now);
    execute(cache, {"TTL", "key"}, now);
    check(cache.stats().hits == hits, "only GET changes hit counters");
    check(execute(cache, {"DEL", "key"}, now) == Message({"INT", "1"}), "DEL returns one");
    check(execute(cache, {"DEL", "key"}, now) == Message({"INT", "0"}), "DEL returns zero");
}
void queue() {
    BoundedQueue<int> queue(2);
    check(queue.push(1) && queue.push(2), "queue admission");
    check(!queue.push(3), "queue rejects overflow");
    check(queue.pop() == 1 && queue.pop() == 2, "queue FIFO order");
    check(!queue.pop(), "empty queue");
    check(queue.size() == 0 && queue.peak() == 2, "queue accounting");
}
}
int main() {
    try {
        policies(); protocol(); commands(); queue();
        std::cout << checks << " checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "Check failed: " << e.what() << '\n';
        return 1;
    }
}
