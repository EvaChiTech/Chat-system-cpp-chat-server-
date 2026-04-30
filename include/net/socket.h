#pragma once
#include "common/platform.h"
#include <string>
#include <cstdint>

namespace chat::net {

// RAII socket wrapper. Cheap to move, non-copyable.
class Socket {
 public:
  Socket() = default;
  explicit Socket(socket_t s) : s_(s) {}
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& o) noexcept : s_(o.s_) { o.s_ = CHAT_SOCKET_INVALID; }
  Socket& operator=(Socket&& o) noexcept {
    if (this != &o) { close(); s_ = o.s_; o.s_ = CHAT_SOCKET_INVALID; }
    return *this;
  }

  socket_t get() const { return s_; }
  socket_t release() { auto t = s_; s_ = CHAT_SOCKET_INVALID; return t; }
  bool valid() const { return socket_valid(s_); }
  void close();

  // Returns true on success.
  static bool set_non_blocking(socket_t s, bool non_blocking);
  static bool set_no_delay   (socket_t s, bool on);
  static bool set_reuse_addr (socket_t s, bool on);
  static bool set_keepalive  (socket_t s, bool on);

  // Bind + listen on (host, port). host="" or "0.0.0.0" binds to all.
  static Socket listen_tcp(const std::string& host, std::uint16_t port,
                           int backlog = 1024);
  // Connect to (host, port). Blocks until connected (or fails).
  static Socket connect_tcp(const std::string& host, std::uint16_t port);

 private:
  socket_t s_ = CHAT_SOCKET_INVALID;
};

}  // namespace chat::net
