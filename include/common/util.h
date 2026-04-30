#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

namespace chat::util {

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
std::string to_lower(std::string s);
std::string to_upper(std::string s);

std::string hex_encode(const std::uint8_t* data, std::size_t n);
std::vector<std::uint8_t> hex_decode(const std::string& s);
std::string base64_encode(const std::uint8_t* data, std::size_t n);
std::vector<std::uint8_t> base64_decode(const std::string& s);

// Cryptographically-strong random bytes (uses std::random_device + mt19937_64
// state expansion; for production replace with a real CSPRNG like /dev/urandom).
void random_bytes(std::uint8_t* out, std::size_t n);

// Monotonic milliseconds since some epoch (steady_clock).
std::int64_t now_steady_ms();

// Wall-clock unix milliseconds.
std::int64_t now_unix_ms();

}  // namespace chat::util
