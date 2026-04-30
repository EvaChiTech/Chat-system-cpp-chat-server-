#pragma once
#include "common/platform.h"
#include "net/reactor.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>

namespace chat::net {

// A length-prefixed-frame TCP connection.
//
// Wire format: [4-byte big-endian length][payload bytes]. Length must not
// exceed `max_frame_bytes`; oversize frames cause the connection to close.
//
// All reads happen on the reactor's I/O thread. send_frame() may be called
// from any thread; it appends to an internal buffer and posts a flush onto
// the reactor.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  using FrameHandler = std::function<void(std::shared_ptr<Connection>,
                                          std::string payload)>;
  using CloseHandler = std::function<void(std::shared_ptr<Connection>)>;

  static std::shared_ptr<Connection> make(socket_t fd, Reactor* reactor,
                                          std::uint32_t max_frame_bytes);

  ~Connection();

  std::uint64_t id() const { return id_; }
  socket_t fd() const { return fd_; }

  void start(FrameHandler on_frame, CloseHandler on_close);
  // Thread-safe. Queues `payload` (already serialized JSON) for sending.
  void send_frame(std::string payload);
  // Schedules the connection to be closed gracefully.
  void close();

  // Used by ChatServer for routing.
  void set_user_id(std::uint64_t uid) { user_id_.store(uid); }
  std::uint64_t user_id() const { return user_id_.load(); }
  void set_session_token(std::string t);
  std::string session_token() const;

 private:
  Connection(socket_t fd, Reactor* r, std::uint32_t max_frame);
  void on_event(int events);
  void on_readable();
  void on_writable();
  void try_flush_locked();        // mu_ held
  void deliver_close();
  void schedule_close();

  static std::atomic<std::uint64_t> next_id_;

  std::uint64_t  id_;
  socket_t       fd_;
  Reactor*       reactor_;
  std::uint32_t  max_frame_;

  // Read state
  std::vector<std::uint8_t> rbuf_;     // accumulating bytes
  bool reading_length_ = true;
  std::uint32_t want_ = 4;

  // Write state
  std::mutex                 mu_;       // protects wbuf_ and write_armed_
  std::vector<std::uint8_t>  wbuf_;
  bool                       write_armed_ = false;
  std::atomic<bool>          closing_{false};
  std::atomic<bool>          closed_{false};

  // App state — user/session bound at auth time.
  std::atomic<std::uint64_t> user_id_{0};
  mutable std::mutex         token_mu_;
  std::string                token_;

  FrameHandler   on_frame_;
  CloseHandler   on_close_;
};

}  // namespace chat::net
