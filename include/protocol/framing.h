#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace chat::protocol {

// Frame an arbitrary payload by prepending a 4-byte big-endian length.
std::string frame(const std::string& payload);

// In-place feed of bytes; returns 0+ complete payloads. Maintains state in
// `state`. Used by the CLI client; the server uses Connection's parser.
struct FrameDecoder {
  std::vector<std::uint8_t> buf;
  bool   reading_len = true;
  std::uint32_t want = 4;
  std::uint32_t max_frame_bytes = 65536;

  // Append `data` and return any frames that completed.
  // Throws std::runtime_error on malformed input.
  std::vector<std::string> feed(const char* data, std::size_t n);
};

}  // namespace chat::protocol
