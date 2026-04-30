// Cross-platform socket / system shim.
// Linux uses BSD sockets + epoll. Windows uses WinSock + select fallback.
#pragma once

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>
  using socket_t = SOCKET;
  inline bool socket_valid(socket_t s) { return s != INVALID_SOCKET; }
  inline int  socket_close(socket_t s) { return ::closesocket(s); }
  inline int  socket_errno()           { return ::WSAGetLastError(); }
  #define CHAT_SOCKET_INVALID INVALID_SOCKET
  #define CHAT_EAGAIN  WSAEWOULDBLOCK
  #define CHAT_EINTR   WSAEINTR
  #define CHAT_ECONNRESET WSAECONNRESET
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  using socket_t = int;
  inline bool socket_valid(socket_t s) { return s >= 0; }
  inline int  socket_close(socket_t s) { return ::close(s); }
  inline int  socket_errno()           { return errno; }
  #define CHAT_SOCKET_INVALID (-1)
  #define CHAT_EAGAIN  EAGAIN
  #define CHAT_EINTR   EINTR
  #define CHAT_ECONNRESET ECONNRESET
#endif

#if defined(__linux__)
  #define CHAT_HAS_EPOLL 1
  #include <sys/epoll.h>
#else
  #define CHAT_HAS_EPOLL 0
#endif

namespace chat::platform {
// Initializes WinSock on Windows, no-op on POSIX.
// Idempotent; safe to call multiple times.
bool init();
void shutdown();
}  // namespace chat::platform
