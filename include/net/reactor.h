#pragma once
#include "common/platform.h"

#include <functional>
#include <memory>
#include <cstdint>

namespace chat::net {

// Single-threaded event reactor. All registered callbacks run on the thread
// that calls run(). Thread-safe operations are post() and stop() (and add /
// modify / remove if called from inside a callback, which is on the I/O thread
// already, or via post()).
class Reactor {
 public:
  enum Mode : int {
    None  = 0,
    Read  = 1 << 0,
    Write = 1 << 1,
  };

  // events is a bitmask of Mode values that fired.
  using Callback = std::function<void(int events)>;

  virtual ~Reactor() = default;

  virtual bool init() = 0;
  virtual void shutdown() = 0;

  // Registers fd with `events` interest. Replaces any prior registration.
  virtual bool add(socket_t fd, int events, Callback cb) = 0;
  // Updates the events mask. Callback is unchanged.
  virtual bool modify(socket_t fd, int events) = 0;
  virtual bool remove(socket_t fd) = 0;

  // Post a closure to run on the I/O thread. Thread-safe; wakes the reactor.
  virtual void post(std::function<void()> fn) = 0;

  // Block, processing events until stop() is called. May only be called once.
  virtual void run() = 0;
  virtual void stop() = 0;

  // Factory: epoll on Linux, select otherwise. Caller owns the result.
  static std::unique_ptr<Reactor> create();
};

}  // namespace chat::net
