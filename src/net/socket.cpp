#include "net/socket.h"
#include "common/logger.h"

#include <cstring>
#include <string>

namespace chat::net {

Socket::~Socket() { close(); }

void Socket::close() {
  if (socket_valid(s_)) {
    socket_close(s_);
    s_ = CHAT_SOCKET_INVALID;
  }
}

bool Socket::set_non_blocking(socket_t s, bool nb) {
#if defined(_WIN32)
  u_long mode = nb ? 1 : 0;
  return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
  int flags = ::fcntl(s, F_GETFL, 0);
  if (flags < 0) return false;
  if (nb) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
  return ::fcntl(s, F_SETFL, flags) == 0;
#endif
}

bool Socket::set_no_delay(socket_t s, bool on) {
  int v = on ? 1 : 0;
  return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

bool Socket::set_reuse_addr(socket_t s, bool on) {
  int v = on ? 1 : 0;
  return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

bool Socket::set_keepalive(socket_t s, bool on) {
  int v = on ? 1 : 0;
  return ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
                      reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
}

Socket Socket::listen_tcp(const std::string& host, std::uint16_t port, int backlog) {
  socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (!socket_valid(s)) {
    LOG_ERROR("listen_tcp: socket() failed errno=" << socket_errno());
    return Socket{};
  }
  set_reuse_addr(s, true);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  if (host.empty() || host == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
      LOG_ERROR("listen_tcp: bad host " << host);
      socket_close(s);
      return Socket{};
    }
  }
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    LOG_ERROR("listen_tcp: bind failed errno=" << socket_errno());
    socket_close(s);
    return Socket{};
  }
  if (::listen(s, backlog) != 0) {
    LOG_ERROR("listen_tcp: listen failed errno=" << socket_errno());
    socket_close(s);
    return Socket{};
  }
  set_non_blocking(s, true);
  return Socket(s);
}

Socket Socket::connect_tcp(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* res = nullptr;
  std::string p = std::to_string(port);
  if (::getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0 || !res) {
    LOG_ERROR("connect_tcp: getaddrinfo failed for " << host);
    return Socket{};
  }
  socket_t s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (!socket_valid(s)) {
    ::freeaddrinfo(res);
    return Socket{};
  }
  if (::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
    LOG_ERROR("connect_tcp: connect failed errno=" << socket_errno());
    socket_close(s);
    ::freeaddrinfo(res);
    return Socket{};
  }
  ::freeaddrinfo(res);
  set_no_delay(s, true);
  return Socket(s);
}

}  // namespace chat::net
