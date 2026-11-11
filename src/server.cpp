#include "voltcache/commands.hpp"
#include "voltcache/queue.hpp"
#include <asio.hpp>
#include <array>
#include <csignal>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <unordered_set>

using asio::ip::tcp;
using namespace voltcache;

namespace {
struct Config {
    std::string host = "127.0.0.1";
    unsigned short port = 6379;
    std::size_t capacity = 10000, queue = 1024, clients = 256;
    unsigned int timeout_ms = 10000, cleanup_ms = 1000;
};

class Session;
class Server {
public:
    Server(asio::io_context& io, const Config& config)
        : io_(io), config_(config), cache_(config.capacity), pending_(config.queue),
          acceptor_(io, tcp::endpoint(asio::ip::make_address(config.host), config.port)), cleanup_(io) {}
    void start();
    void stop();
    void submit(Message request, std::function<void(Message)> reply);
    void departed(const std::shared_ptr<Session>& session) { sessions_.erase(session); }
    unsigned int timeout_ms() const { return config_.timeout_ms; }
    unsigned short port() const { return acceptor_.local_endpoint().port(); }
private:
    struct Work { Message request; std::function<void(Message)> reply; };
    void accept();
    void drain();
    void sweep();
    asio::io_context& io_;
    Config config_;
    Cache cache_;
    BoundedQueue<Work> pending_;
    tcp::acceptor acceptor_;
    asio::steady_timer cleanup_;
    std::unordered_set<std::shared_ptr<Session>> sessions_;
    bool draining_ = false, stopped_ = false;
    std::uint64_t commands_ = 0, busy_ = 0, rejected_clients_ = 0;
    Cache::Time started_ = Cache::Clock::now();
};

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, Server& server)
        : socket_(std::move(socket)), server_(server), deadline_(socket_.get_executor()) {}
    void start() { read_header(); }
    void close() {
        if (closed_) return;
        closed_ = true;
        deadline_.cancel();
        asio::error_code ignored;
        socket_.close(ignored);
        server_.departed(shared_from_this());
    }
private:
    void arm_deadline() {
        deadline_.expires_after(std::chrono::milliseconds(server_.timeout_ms()));
        auto self = shared_from_this();
        deadline_.async_wait([self](asio::error_code ec) { if (!ec) self->close(); });
    }
    void read_header() {
        if (closed_) return;
        arm_deadline(); // One deadline covers the entire incoming frame.
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(header_), [self](asio::error_code ec, std::size_t) {
            if (ec) return self->close();
            auto length = read_u32(self->header_.data());
            if (length < 2 || length > max_frame)
                return self->write({"ERROR", "FRAME", "frame length must be 2 to 65536 bytes"}, true);
            self->payload_.resize(length);
            self->read_body();
        });
    }
    void read_body() {
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(payload_), [self](asio::error_code ec, std::size_t) {
            if (ec) return self->close();
            Message request;
            try { request = decode(self->payload_); }
            catch (const std::invalid_argument& e) { return self->write({"ERROR", "FRAME", e.what()}, true); }
            self->deadline_.cancel();
            // At most one queued command per connection; response order is preserved.
            self->server_.submit(std::move(request), [weak = std::weak_ptr<Session>(self)](Message response) {
                if (auto session = weak.lock()) session->write(std::move(response));
            });
        });
    }
    void write(Message response, bool close_after = false) {
        if (closed_) return;
        output_ = frame(response);
        arm_deadline();
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(output_), [self, close_after](asio::error_code ec, std::size_t) {
            if (ec || close_after) return self->close();
            self->read_header();
        });
    }
    tcp::socket socket_;
    Server& server_;
    asio::steady_timer deadline_;
    std::array<unsigned char, 4> header_{};
    std::string payload_, output_;
    bool closed_ = false;
};

void Server::start() { accept(); sweep(); }

void Server::accept() {
    acceptor_.async_accept([this](asio::error_code ec, tcp::socket socket) {
        if (stopped_) return;
        if (!ec) {
            if (sessions_.size() >= config_.clients) {
                ++rejected_clients_;
                asio::error_code ignored;
                socket.close(ignored); // No session allocation beyond the configured limit.
            } else {
                socket.set_option(tcp::no_delay(true));
                auto session = std::make_shared<Session>(std::move(socket), *this);
                sessions_.insert(session);
                session->start();
            }
        } else if (ec != asio::error::operation_aborted) {
            // Fail explicitly rather than spinning if the OS cannot accept sockets.
            throw asio::system_error(ec, "accept failed");
        }
        accept();
    });
}

void Server::submit(Message request, std::function<void(Message)> reply) {
    // Keep the callback intact if admission fails.
    if (!pending_.push(Work{std::move(request), reply})) {
        ++busy_;
        reply({"ERROR", "BUSY", "request queue is full; retry later"});
        return;
    }
    if (!draining_) {
        draining_ = true;
        asio::post(io_, [this] { drain(); });
    }
}

void Server::drain() {
    if (stopped_) return;
    auto work = pending_.pop();
    if (!work) { draining_ = false; return; }
    ++commands_;
    auto result = execute(cache_, std::move(work->request));
    if (result[0] == "STATS") {
        auto add = [&result](const char* key, auto value) {
            result.emplace_back(key); result.push_back(std::to_string(value));
        };
        add("connected_clients", sessions_.size());
        add("queue_depth", pending_.size());
        add("queue_peak", pending_.peak());
        add("queue_capacity", config_.queue);
        add("commands", commands_);
        add("busy_responses", busy_);
        add("rejected_clients", rejected_clients_);
        add("uptime_seconds", std::chrono::duration_cast<std::chrono::seconds>(Cache::Clock::now() - started_).count());
    }
    work->reply(std::move(result));
    // Yield between commands so reads, timeouts, and new connections can run.
    asio::post(io_, [this] { drain(); });
}

void Server::sweep() {
    cleanup_.expires_after(std::chrono::milliseconds(config_.cleanup_ms));
    cleanup_.async_wait([this](asio::error_code ec) {
        if (ec || stopped_) return;
        cache_.cleanup();
        sweep();
    });
}

void Server::stop() {
    if (stopped_) return;
    stopped_ = true;
    asio::error_code ignored;
    acceptor_.close(ignored);
    cleanup_.cancel();
    while (!sessions_.empty()) (*sessions_.begin())->close();
}

void usage() {
    std::cout << "Usage: voltcache-server [options]\n"
              << "  --host ADDRESS       bind address (127.0.0.1)\n"
              << "  --port N             TCP port (6379; 0 selects a free port)\n"
              << "  --max-entries N      maximum cache entries (10000)\n"
              << "  --queue-capacity N   pending commands (1024)\n"
              << "  --max-clients N      connected clients (256)\n"
              << "  --timeout-ms N       complete frame read/write deadline (10000)\n"
              << "  --cleanup-ms N       expiration sweep interval (1000)\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        Config config;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") { usage(); return 0; }
            if (i + 1 >= argc) throw std::invalid_argument("missing option value");
            std::string value = argv[++i];
            if (arg == "--host") config.host = value;
            else if (arg == "--port") config.port = static_cast<unsigned short>(number(value, 0, 65535));
            else if (arg == "--max-entries") config.capacity = number(value, 1, 1000000);
            else if (arg == "--queue-capacity") config.queue = number(value, 1, 65536);
            else if (arg == "--max-clients") config.clients = number(value, 1, 4096);
            else if (arg == "--timeout-ms") config.timeout_ms = static_cast<unsigned int>(number(value, 10, 3600000));
            else if (arg == "--cleanup-ms") config.cleanup_ms = static_cast<unsigned int>(number(value, 10, 3600000));
            else throw std::invalid_argument("unknown option: " + arg);
        }
        asio::io_context io;
        Server server(io, config);
        asio::signal_set signals(io, SIGINT, SIGTERM);
        signals.async_wait([&](asio::error_code ec, int) { if (!ec) server.stop(); });
        server.start();
        std::cout << "VoltCache listening on " << config.host << ':' << server.port()
                  << " | capacity " << config.capacity << " | LRU" << std::endl;
        io.run();
    } catch (const std::exception& e) {
        std::cerr << "voltcache-server: " << e.what() << '\n';
        return 1;
    }
}
