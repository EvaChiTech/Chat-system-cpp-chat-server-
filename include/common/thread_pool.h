#pragma once
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>
#include <future>

namespace chat {

// A simple, fixed-size thread pool. Tasks are std::function<void()>;
// callers wrap futures themselves if they need a result.
//
// Properties:
//   - lock-protected FIFO queue
//   - graceful shutdown drains queue then joins
//   - submit() returns false if pool is stopping (rejected work)
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t n);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  bool submit(std::function<void()> task);
  std::size_t size() const { return workers_.size(); }
  std::size_t pending() const;
  void shutdown();   // idempotent; blocks until workers join

 private:
  void worker_loop();

  std::vector<std::thread>            workers_;
  mutable std::mutex                  mu_;
  std::condition_variable             cv_;
  std::queue<std::function<void()>>   q_;
  std::atomic<bool>                   stopping_{false};
};

}  // namespace chat
