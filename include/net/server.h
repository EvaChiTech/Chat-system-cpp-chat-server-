#pragma once
#include "common/platform.h"
#include "net/reactor.h"
#include "net/connection.h"
#include "net/socket.h"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>

namespace chat::net {

// A TCP listener that accepts new connections, wraps them as Connection
// objects, and dispatches frame events to a single user-supplied handler.
class Server {
 public:
  using FrameHandler = std::function<void(std::shared_ptr<Connection>,
                                          std::string payload)>;
  using ConnectHandler = std::function<void(std::shared_ptr<Connection>)>;
  using CloseHandler   = std::function<void(std::shared_ptr<Connection>)>;

  Server(Reactor* reactor, std::uint32_t max_frame_bytes);
  ~Server();

  bool listen(const std::string& host, std::uint16_t port);
  void set_handlers(ConnectHandler on_connect,
                    FrameHandler on_frame,
                    CloseHandler on_close);

  // Stop accepting and tear down all live connections.
  void stop();

  std::size_t connection_count() const;

 private:
  void on_listen_event(int events);
  void register_connection(std::shared_ptr<Connection> c);
  void unregister_connection(std::shared_ptr<Connection> c);

  Reactor*               reactor_;
  std::uint32_t          max_frame_;
  Socket                 listener_;
  ConnectHandler         on_connect_;
  FrameHandler           on_frame_;
  CloseHandler           on_close_;

  mutable std::mutex     mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> conns_;
};

}  // namespace chat::net
