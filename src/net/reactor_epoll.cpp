// Linux-only epoll reactor. On non-Linux this file compiles to nothing.
#include "net/reactor.h"
#include "common/logger.h"

#if CHAT_HAS_EPOLL

#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstring>

namespace chat::net {

class EpollReactor final : public Reactor {
 public:
  bool init() override {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
      LOG_ERROR("epoll_create1 failed errno=" << errno);
      return false;
    }
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
      LOG_ERROR("eventfd failed errno=" << errno);
      return false;
    }
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = wake_fd_;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, wake_fd_, &ev) != 0) {
      LOG_ERROR("epoll_ctl wake_fd failed errno=" << errno);
      return false;
    }
    return true;
  }

  void shutdown() override {
    if (epfd_ >= 0)   { ::close(epfd_);   epfd_ = -1; }
    if (wake_fd_ >= 0){ ::close(wake_fd_);wake_fd_ = -1; }
    cbs_.clear();
  }

  bool add(socket_t fd, int events, Callback cb) override {
    epoll_event ev{};
    ev.events = to_epoll(events) | EPOLLET;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
      // Maybe already registered: try MOD.
      if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) != 0) {
        LOG_ERROR("epoll add fd=" << fd << " failed errno=" << errno);
        return false;
      }
    }
    cbs_[fd] = std::move(cb);
    return true;
  }

  bool modify(socket_t fd, int events) override {
    epoll_event ev{};
    ev.events  = to_epoll(events) | EPOLLET;
    ev.data.fd = fd;
    return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
  }

  bool remove(socket_t fd) override {
    cbs_.erase(fd);
    // EPOLL_CTL_DEL with closed fd returns EBADF; that's fine.
    return ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
  }

  void post(std::function<void()> fn) override {
    {
      std::lock_guard<std::mutex> lk(post_mu_);
      pending_.push_back(std::move(fn));
    }
    std::uint64_t one = 1;
    // The eventfd write only fails if the counter would overflow; for our
    // wake-up signal that's impossible. Capture the result so glibc's
    // __warn_unused_result__ on write() doesn't fire under -Werror.
    ssize_t w = ::write(wake_fd_, &one, sizeof(one));
    (void)w;
  }

  void run() override {
    constexpr int kMaxEvents = 256;
    epoll_event events[kMaxEvents];
    while (!stopping_.load(std::memory_order_acquire)) {
      int n = ::epoll_wait(epfd_, events, kMaxEvents, /*timeout_ms*/ 1000);
      if (n < 0) {
        if (errno == EINTR) continue;
        LOG_ERROR("epoll_wait failed errno=" << errno);
        break;
      }
      for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        if (fd == wake_fd_) {
          std::uint64_t v;
          ssize_t r = ::read(wake_fd_, &v, sizeof(v));
          (void)r;
          drain_posts();
          continue;
        }
        int mask = 0;
        if (events[i].events & (EPOLLIN | EPOLLPRI)) mask |= Read;
        if (events[i].events & EPOLLOUT)             mask |= Write;
        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
          mask |= Read;  // surface as readable; handler will see EOF
        auto it = cbs_.find(fd);
        if (it != cbs_.end()) {
          // Copy the callback in case the user mutates cbs_ from inside.
          auto cb = it->second;
          cb(mask);
        }
      }
    }
  }

  void stop() override {
    stopping_.store(true, std::memory_order_release);
    std::uint64_t one = 1;
    if (wake_fd_ >= 0) {
      ssize_t w = ::write(wake_fd_, &one, sizeof(one));
      (void)w;
    }
  }

 private:
  static std::uint32_t to_epoll(int events) {
    std::uint32_t e = 0;
    if (events & Read)  e |= EPOLLIN | EPOLLRDHUP;
    if (events & Write) e |= EPOLLOUT;
    return e;
  }

  void drain_posts() {
    std::vector<std::function<void()>> batch;
    {
      std::lock_guard<std::mutex> lk(post_mu_);
      batch.swap(pending_);
    }
    for (auto& fn : batch) {
      try { fn(); } catch (...) {}
    }
  }

  int  epfd_   = -1;
  int  wake_fd_= -1;
  std::atomic<bool> stopping_{false};
  std::unordered_map<int, Callback> cbs_;
  std::mutex post_mu_;
  std::vector<std::function<void()>> pending_;
};

// Factory hook: forward-declared in reactor_select.cpp.
std::unique_ptr<Reactor> make_epoll_reactor() {
  return std::unique_ptr<Reactor>(new EpollReactor());
}

}  // namespace chat::net

#else
namespace chat::net {
// Stub so the symbol exists on non-Linux; never called by Reactor::create().
std::unique_ptr<Reactor> make_epoll_reactor() { return nullptr; }
}
#endif  // CHAT_HAS_EPOLL
