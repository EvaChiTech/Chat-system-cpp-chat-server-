#include "test_runner.h"
#include "common/thread_pool.h"

#include <atomic>
#include <chrono>
#include <thread>

TEST(thread_pool_runs_tasks) {
  chat::ThreadPool pool(4);
  std::atomic<int> n{0};
  for (int i = 0; i < 1000; ++i) {
    pool.submit([&]{ n.fetch_add(1, std::memory_order_relaxed); });
  }
  // Spin-wait — better than sleep on slow CI.
  for (int i = 0; i < 5000 && n.load() < 1000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(n.load(), 1000);
}

TEST(thread_pool_rejects_after_shutdown) {
  chat::ThreadPool pool(2);
  pool.shutdown();
  bool ok = pool.submit([]{});
  CHECK(!ok);
}

TEST(thread_pool_swallows_exceptions) {
  chat::ThreadPool pool(2);
  pool.submit([]{ throw std::runtime_error("boom"); });
  std::atomic<int> ran{0};
  for (int i = 0; i < 50; ++i) {
    pool.submit([&]{ ran.fetch_add(1); });
  }
  for (int i = 0; i < 1000 && ran.load() < 50; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(ran.load(), 50);
}
