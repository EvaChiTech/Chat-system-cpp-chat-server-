#include "common/thread_pool.h"

namespace chat {

ThreadPool::ThreadPool(std::size_t n) {
  if (n == 0) n = 1;
  workers_.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::submit(std::function<void()> task) {
  if (stopping_.load(std::memory_order_acquire)) return false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (stopping_.load()) return false;
    q_.push(std::move(task));
  }
  cv_.notify_one();
  return true;
}

std::size_t ThreadPool::pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return q_.size();
}

void ThreadPool::shutdown() {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true)) {
    // Already stopping; still wait for joiners.
  }
  cv_.notify_all();
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
}

void ThreadPool::worker_loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return stopping_.load() || !q_.empty(); });
      if (q_.empty()) {
        if (stopping_.load()) return;
        continue;
      }
      job = std::move(q_.front());
      q_.pop();
    }
    try {
      job();
    } catch (...) {
      // Swallow exceptions so a single bad task doesn't kill the worker.
    }
  }
}

}  // namespace chat
