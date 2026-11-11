#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voltcache {
using Message = std::vector<std::string>;
inline constexpr std::size_t max_frame = 65536;
inline constexpr std::size_t max_key = 256;
inline constexpr std::size_t max_value = 60000;
inline constexpr std::uint64_t max_ttl = 31536000; // One year.
std::uint32_t read_u32(const unsigned char* bytes);
std::string frame(const Message& fields);
Message decode(std::string_view payload);
Message parse_command(std::string_view line);
std::uint64_t number(std::string_view value, std::uint64_t min, std::uint64_t max);
} // namespace voltcache
