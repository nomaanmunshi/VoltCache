#include "voltcache/protocol.hpp"
#include <asio.hpp>
#include <array>
#include <iomanip>
#include <iostream>

using namespace voltcache;
using asio::ip::tcp;

namespace {
class Client {
public:
    Client(const std::string& host, unsigned short port, unsigned int timeout)
        : socket_(io_), timer_(io_), timeout_(timeout) {
        auto endpoint = tcp::endpoint(asio::ip::make_address(host), port);
        run([&](auto done) { socket_.async_connect(endpoint, [done](asio::error_code ec) { done(ec); }); });
        socket_.set_option(tcp::no_delay(true));
    }
    Message request(const Message& fields) {
        auto bytes = frame(fields);
        run([&](auto done) {
            asio::async_write(socket_, asio::buffer(bytes), [done](asio::error_code ec, std::size_t) { done(ec); });
        });
        std::array<unsigned char, 4> header{};
        run([&](auto done) {
            asio::async_read(socket_, asio::buffer(header), [done](asio::error_code ec, std::size_t) { done(ec); });
        });
        auto length = read_u32(header.data());
        if (length < 2 || length > max_frame) throw std::runtime_error("invalid response size");
        std::string payload(length, '\0');
        run([&](auto done) {
            asio::async_read(socket_, asio::buffer(payload), [done](asio::error_code ec, std::size_t) { done(ec); });
        });
        return decode(payload);
    }
private:
    template <typename Start> void run(Start start) {
        io_.restart();
        asio::error_code result;
        bool timed_out = false;
        timer_.expires_after(std::chrono::milliseconds(timeout_));
        timer_.async_wait([&](asio::error_code ec) {
            if (!ec) {
                timed_out = true;
                asio::error_code ignored;
                socket_.close(ignored);
            }
        });
        start([&](asio::error_code ec) { result = ec; timer_.cancel(); });
        io_.run();
        if (timed_out) throw std::runtime_error("request timed out");
        if (result) throw asio::system_error(result);
    }
    asio::io_context io_;
    tcp::socket socket_;
    asio::steady_timer timer_;
    unsigned int timeout_;
};

void print_value(const std::string& value) {
    std::cout << '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': std::cout << "\\\""; break;
            case '\\': std::cout << "\\\\"; break;
            case '\n': std::cout << "\\n"; break;
            case '\r': std::cout << "\\r"; break;
            case '\t': std::cout << "\\t"; break;
            default:
                if (c < 32 || c == 127) {
                    const char* hex = "0123456789abcdef";
                    std::cout << "\\x" << hex[c >> 4] << hex[c & 15];
                } else std::cout << static_cast<char>(c);
        }
    }
    std::cout << "\"\n";
}

bool display(const Message& response) {
    if (response[0] == "VALUE" && response.size() == 2) print_value(response[1]);
    else if (response[0] == "INT" && response.size() == 2) std::cout << response[1] << '\n';
    else if (response[0] == "STATS" && response.size() % 2 == 1) {
        for (std::size_t i = 1; i + 1 < response.size(); i += 2)
            std::cout << response[i] << ": " << response[i + 1] << '\n';
    } else if (response[0] == "ERROR" && response.size() == 3) {
        std::cerr << response[1] << ": " << response[2] << '\n';
        return false;
    } else if (response.size() == 1) std::cout << response[0] << '\n';
    else throw std::runtime_error("unexpected response");
    return true;
}
}

int main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1";
        unsigned short port = 6379;
        unsigned int timeout = 3000;
        Message command;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (command.empty() && arg == "--help") {
                std::cout << "Usage: voltcache-cli [--host ADDRESS] [--port N] [--timeout-ms N] [COMMAND args...]\n"
                          << "Without a command, opens an interactive prompt. QUIT exits.\n"
                          << "Commands: PING, SET key value [EX seconds], GET key, DEL key, EXISTS key, EXPIRE key seconds, TTL key, STATS\n";
                return 0;
            }
            if (command.empty() && (arg == "--host" || arg == "--port" || arg == "--timeout-ms")) {
                if (i + 1 >= argc) throw std::invalid_argument("missing option value");
                std::string value = argv[++i];
                if (arg == "--host") host = value;
                else if (arg == "--port") port = static_cast<unsigned short>(number(value, 1, 65535));
                else timeout = static_cast<unsigned int>(number(value, 10, 3600000));
            } else command.push_back(std::move(arg));
        }
        if (!command.empty()) {
            Client client(host, port, timeout);
            return display(client.request(command)) ? 0 : 1;
        }
        std::string line;
        while (std::cout << "volt> " && std::getline(std::cin, line)) {
            try {
                auto fields = parse_command(line);
                if (fields.empty()) continue;
                if (fields.size() == 1 && (fields[0] == "QUIT" || fields[0] == "quit")) break;
                // A prompt can stay idle for minutes without holding a server slot.
                Client client(host, port, timeout);
                display(client.request(fields));
            } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
        }
    } catch (const std::exception& e) {
        std::cerr << "voltcache-cli: " << e.what() << '\n';
        return 1;
    }
}
