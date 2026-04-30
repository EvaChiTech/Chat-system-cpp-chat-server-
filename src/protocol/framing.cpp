#include "protocol/framing.h"
#include <stdexcept>
#include <cstring>

namespace chat::protocol {

std::string frame(const std::string& payload) {
  std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  std::string out;
  out.resize(4 + payload.size());
  out[0] = static_cast<char>((len >> 24) & 0xFF);
  out[1] = static_cast<char>((len >> 16) & 0xFF);
  out[2] = static_cast<char>((len >> 8)  & 0xFF);
  out[3] = static_cast<char>( len        & 0xFF);
  std::memcpy(out.data() + 4, payload.data(), payload.size());
  return out;
}

std::vector<std::string> FrameDecoder::feed(const char* data, std::size_t n) {
  std::vector<std::string> frames;
  buf.insert(buf.end(),
             reinterpret_cast<const std::uint8_t*>(data),
             reinterpret_cast<const std::uint8_t*>(data) + n);
  for (;;) {
    if (reading_len) {
      if (buf.size() < 4) break;
      std::uint32_t len = (static_cast<std::uint32_t>(buf[0]) << 24) |
                          (static_cast<std::uint32_t>(buf[1]) << 16) |
                          (static_cast<std::uint32_t>(buf[2]) << 8)  |
                          (static_cast<std::uint32_t>(buf[3]));
      if (len == 0) throw std::runtime_error("zero-length frame");
      if (len > max_frame_bytes)
        throw std::runtime_error("frame exceeds max_frame_bytes");
      want = len;
      buf.erase(buf.begin(), buf.begin() + 4);
      reading_len = false;
    }
    if (buf.size() < want) break;
    frames.emplace_back(reinterpret_cast<const char*>(buf.data()), want);
    buf.erase(buf.begin(), buf.begin() + want);
    reading_len = true;
    want = 4;
  }
  return frames;
}

}  // namespace chat::protocol
