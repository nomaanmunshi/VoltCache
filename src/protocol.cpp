#include "voltcache/protocol.hpp"
#include <charconv>
#include <stdexcept>

namespace voltcache {
namespace {
void append_u32(std::string& out, std::uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((n >> shift) & 0xff));
}
}

std::uint32_t read_u32(const unsigned char* b) {
    return (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) |
           (std::uint32_t(b[2]) << 8) | std::uint32_t(b[3]);
}

std::string frame(const Message& fields) {
    if (fields.empty() || fields.size() > 64) throw std::invalid_argument("invalid field count");
    std::string payload;
    payload.push_back(static_cast<char>(fields.size() >> 8));
    payload.push_back(static_cast<char>(fields.size() & 0xff));
    for (const auto& field : fields) {
        if (field.size() > max_frame || payload.size() + 4 + field.size() > max_frame)
            throw std::invalid_argument("frame too large");
        append_u32(payload, static_cast<std::uint32_t>(field.size()));
        payload += field;
    }
    std::string result;
    append_u32(result, static_cast<std::uint32_t>(payload.size()));
    result += payload;
    return result;
}

Message decode(std::string_view payload) {
    if (payload.size() < 2 || payload.size() > max_frame) throw std::invalid_argument("invalid frame size");
    const auto* bytes = reinterpret_cast<const unsigned char*>(payload.data());
    auto count = (std::uint32_t(bytes[0]) << 8) | bytes[1];
    if (count == 0 || count > 64) throw std::invalid_argument("invalid field count");
    Message fields;
    std::size_t pos = 2;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (payload.size() - pos < 4) throw std::invalid_argument("missing field length");
        auto length = read_u32(bytes + pos);
        pos += 4;
        if (length > payload.size() - pos) throw std::invalid_argument("truncated field");
        fields.emplace_back(payload.substr(pos, length));
        pos += length;
    }
    if (pos != payload.size()) throw std::invalid_argument("trailing bytes");
    return fields;
}

std::uint64_t number(std::string_view value, std::uint64_t min, std::uint64_t max) {
    std::uint64_t n = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (error != std::errc{} || end != value.data() + value.size() || n < min || n > max)
        throw std::invalid_argument("number out of range");
    return n;
}

Message parse_command(std::string_view line) {
    Message fields;
    std::string field;
    char quote = 0;
    bool started = false, escaped = false;
    for (char c : line) {
        if (escaped) {
            switch (c) {
                case 'n': field += '\n'; break;
                case 'r': field += '\r'; break;
                case 't': field += '\t'; break;
                default: field += c;
            }
            escaped = false;
            started = true;
        } else if (c == '\\') { escaped = true; started = true; }
        else if (quote) { if (c == quote) quote = 0; else field += c; }
        else if (c == '"' || c == '\'') { quote = c; started = true; }
        else if (c == ' ' || c == '\t' || c == '\r') {
            if (started) { fields.push_back(std::move(field)); field.clear(); started = false; }
        } else { field += c; started = true; }
    }
    if (quote || escaped) throw std::invalid_argument("unfinished quote or escape");
    if (started) fields.push_back(std::move(field));
    return fields;
}
} // namespace voltcache
