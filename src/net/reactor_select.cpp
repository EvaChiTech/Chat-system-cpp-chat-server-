// select()-based reactor — works on Windows and any POSIX. Lower scaling
// ceiling than epoll (FD_SETSIZE), but adequate as a fallback or for Windows.
#include "net/reactor.h"
#include "net/socket.h"
#include "common/logger.h"

#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>

namespace chat::net {

namespace { struct Entry { int events; Reactor::Callback cb; }; }

class SelectReactor final : public Reactor {
 public:
  bool init() override {
    if (!chat::platform::init()) return false;
    if (!make_wake_pair()) return false;
    return true;
  }

  void shutdown() override {
    cbs_.clear();
    if (socket_valid(wake_r_)) socket_close(wake_r_);
    if (socket_valid(wake_w_)) socket_close(wake_w_);
    wake_r_ = wake_w_ = CHAT_SOCKET_INVALID;
  }

  bool add(socket_t fd, int events, Callback cb) override {
    cbs_[fd] = Entry{events, std::move(cb)};
    return true;
  }

  bool modify(socket_t fd, int events) override {
    auto it = cbs_.find(fd);
    if (it == cbs_.end()) return false;
    it->second.events = events;
    return true;
  }

  bool remove(socket_t fd) override {
    cbs_.erase(fd);
    return true;
  }

  void post(std::function<void()> fn) override {
    {
      std::lock_guard<std::mutex> lk(post_mu_);
      pending_.push_back(std::move(fn));
    }
    char b = 1;
    ::send(wake_w_, &b, 1, 0);
  }

  void run() override {
    while (!stopping_.load(std::memory_order_acquire)) {
      fd_set rfds, wfds;
      FD_ZERO(&rfds);
      FD_ZERO(&wfds);
      socket_t maxfd = wake_r_;
      FD_SET(wake_r_, &rfds);
      for (auto& [fd, e] : cbs_) {
        if (e.events & Read)  FD_SET(fd, &rfds);
        if (e.events & Write) FD_SET(fd, &wfds);
        if (fd > maxfd) maxfd = fd;
      }
      timeval tv{1, 0};
      int n = ::select(static_cast<int>(maxfd) + 1, &rfds, &wfds, nullptr, &tv);
      if (n < 0) {
        int e = socket_errno();
        if (e == CHAT_EINTR) continue;
        LOG_ERROR("select failed errno=" << e);
        break;
      }
      if (n == 0) continue;

      if (FD_ISSET(wake_r_, &rfds)) {
        char buf[64];
        while (::recv(wake_r_, buf, sizeof(buf), 0) > 0) {}
        drain_posts();
      }
      // Snapshot to avoid invalidation if cbs_ mutates.
      std::vector<std::pair<socket_t, int>> ready;
      ready.reserve(cbs_.size());
      for (auto& [fd, _] : cbs_) {
        int m = 0;
        if (FD_ISSET(fd, &rfds)) m |= Read;
        if (FD_ISSET(fd, &wfds)) m |= Write;
        if (m) ready.emplace_back(fd, m);
      }
      for (auto [fd, m] : ready) {
        auto it = cbs_.find(fd);
        if (it == cbs_.end()) continue;
        auto cb = it->second.cb;
        cb(m);
      }
    }
  }

  void stop() override {
    stopping_.store(true, std::memory_order_release);
    char b = 0;
    if (socket_valid(wake_w_)) ::send(wake_w_, &b, 1, 0);
  }

 private:
  bool make_wake_pair() {
    // Loopback TCP self-pair, works on Windows and POSIX.
    socket_t l = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!socket_valid(l)) return false;
    Socket::set_reuse_addr(l, true);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(l, (sockaddr*)&addr, sizeof(addr)) != 0) { socket_close(l); return false; }
    socklen_t alen = sizeof(addr);
    if (::getsockname(l, (sockaddr*)&addr, &alen) != 0) { socket_close(l); return false; }
    if (::listen(l, 1) != 0) { socket_close(l); return false; }

    socket_t c = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!socket_valid(c)) { socket_close(l); return false; }
    if (::connect(c, (sockaddr*)&addr, sizeof(addr)) != 0) {
      socket_close(l); socket_close(c); return false;
    }
    socket_t s = ::accept(l, nullptr, nullptr);
    socket_close(l);
    if (!socket_valid(s)) { socket_close(c); return false; }

    Socket::set_non_blocking(c, true);
    Socket::set_non_blocking(s, true);
    wake_w_ = c;
    wake_r_ = s;
    return true;
  }

  void drain_posts() {
    std::vector<std::function<void()>> batch;
    {
      std::lock_guard<std::mutex> lk(post_mu_);
      batch.swap(pending_);
    }
    for (auto& fn : batch) { try { fn(); } catch (...) {} }
  }

  std::unordered_map<socket_t, Entry> cbs_;
  socket_t wake_r_ = CHAT_SOCKET_INVALID;
  socket_t wake_w_ = CHAT_SOCKET_INVALID;
  std::atomic<bool> stopping_{false};
  std::mutex post_mu_;
  std::vector<std::function<void()>> pending_;
};

// Factory wiring.
std::unique_ptr<Reactor> make_select_reactor() {
  return std::unique_ptr<Reactor>(new SelectReactor());
}

#if CHAT_HAS_EPOLL
extern std::unique_ptr<Reactor> make_epoll_reactor();
#endif

std::unique_ptr<Reactor> Reactor::create() {
#if CHAT_HAS_EPOLL
  return make_epoll_reactor();
#else
  return make_select_reactor();
#endif
}

}  // namespace chat::net
