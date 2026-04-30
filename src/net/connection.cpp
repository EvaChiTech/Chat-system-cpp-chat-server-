#include "net/connection.h"
#include "net/socket.h"
#include "common/logger.h"

#include <cstring>

namespace chat::net {

std::atomic<std::uint64_t> Connection::next_id_{1};

std::shared_ptr<Connection> Connection::make(socket_t fd, Reactor* r,
                                             std::uint32_t max_frame) {
  // can't use std::make_shared on a private ctor; use new + shared_ptr.
  return std::shared_ptr<Connection>(new Connection(fd, r, max_frame));
}

Connection::Connection(socket_t fd, Reactor* r, std::uint32_t max_frame)
    : id_(next_id_.fetch_add(1)), fd_(fd), reactor_(r), max_frame_(max_frame) {
  Socket::set_non_blocking(fd_, true);
  Socket::set_no_delay(fd_, true);
  Socket::set_keepalive(fd_, true);
  rbuf_.reserve(8192);
}

Connection::~Connection() {
  if (socket_valid(fd_)) {
    socket_close(fd_);
    fd_ = CHAT_SOCKET_INVALID;
  }
}

void Connection::start(FrameHandler on_frame, CloseHandler on_close) {
  on_frame_ = std::move(on_frame);
  on_close_ = std::move(on_close);
  auto self = shared_from_this();
  reactor_->add(fd_, Reactor::Read, [self](int ev) { self->on_event(ev); });
}

void Connection::set_session_token(std::string t) {
  std::lock_guard<std::mutex> lk(token_mu_);
  token_ = std::move(t);
}

std::string Connection::session_token() const {
  std::lock_guard<std::mutex> lk(token_mu_);
  return token_;
}

void Connection::on_event(int events) {
  if (closed_.load()) return;
  if (events & Reactor::Read)  on_readable();
  if (closed_.load()) return;
  if (events & Reactor::Write) on_writable();
}

void Connection::on_readable() {
  std::uint8_t tmp[4096];
  for (;;) {
    auto n = ::recv(fd_, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
    if (n > 0) {
      rbuf_.insert(rbuf_.end(), tmp, tmp + n);
      // Parse all complete frames in buffer.
      for (;;) {
        if (reading_length_) {
          if (rbuf_.size() < 4) break;
          std::uint32_t len = (static_cast<std::uint32_t>(rbuf_[0]) << 24) |
                              (static_cast<std::uint32_t>(rbuf_[1]) << 16) |
                              (static_cast<std::uint32_t>(rbuf_[2]) << 8)  |
                              (static_cast<std::uint32_t>(rbuf_[3]));
          if (len == 0 || len > max_frame_) {
            LOG_WARN("conn[" << id_ << "] bad frame len=" << len);
            schedule_close();
            return;
          }
          want_ = len;
          rbuf_.erase(rbuf_.begin(), rbuf_.begin() + 4);
          reading_length_ = false;
        }
        if (rbuf_.size() < want_) break;
        std::string payload(reinterpret_cast<const char*>(rbuf_.data()), want_);
        rbuf_.erase(rbuf_.begin(), rbuf_.begin() + want_);
        reading_length_ = true;
        want_ = 4;
        if (on_frame_) {
          try { on_frame_(shared_from_this(), std::move(payload)); }
          catch (const std::exception& e) {
            LOG_ERROR("conn[" << id_ << "] handler threw: " << e.what());
          }
        }
      }
    } else if (n == 0) {
      // EOF
      schedule_close();
      return;
    } else {
      int e = socket_errno();
      if (e == CHAT_EAGAIN) return;          // drained
      if (e == CHAT_EINTR)  continue;
      LOG_DEBUG("conn[" << id_ << "] recv errno=" << e);
      schedule_close();
      return;
    }
  }
}

void Connection::send_frame(std::string payload) {
  if (closing_.load() || closed_.load()) return;
  if (payload.size() > max_frame_) {
    LOG_WARN("conn[" << id_ << "] dropping outbound frame, too large");
    return;
  }
  std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  std::uint8_t hdr[4] = {
      static_cast<std::uint8_t>((len >> 24) & 0xFF),
      static_cast<std::uint8_t>((len >> 16) & 0xFF),
      static_cast<std::uint8_t>((len >> 8)  & 0xFF),
      static_cast<std::uint8_t>( len        & 0xFF),
  };
  {
    std::lock_guard<std::mutex> lk(mu_);
    wbuf_.insert(wbuf_.end(), hdr, hdr + 4);
    wbuf_.insert(wbuf_.end(),
                 reinterpret_cast<const std::uint8_t*>(payload.data()),
                 reinterpret_cast<const std::uint8_t*>(payload.data()) +
                     payload.size());
  }
  // Run the actual flush on the reactor thread.
  auto self = shared_from_this();
  reactor_->post([self] {
    if (self->closed_.load()) return;
    std::lock_guard<std::mutex> lk(self->mu_);
    self->try_flush_locked();
  });
}

void Connection::try_flush_locked() {
  while (!wbuf_.empty()) {
    auto n = ::send(fd_, reinterpret_cast<const char*>(wbuf_.data()),
                    wbuf_.size(), 0);
    if (n > 0) {
      wbuf_.erase(wbuf_.begin(), wbuf_.begin() + n);
    } else {
      int e = socket_errno();
      if (e == CHAT_EAGAIN) {
        if (!write_armed_) {
          reactor_->modify(fd_, Reactor::Read | Reactor::Write);
          write_armed_ = true;
        }
        return;
      }
      if (e == CHAT_EINTR) continue;
      LOG_DEBUG("conn[" << id_ << "] send errno=" << e);
      // schedule close from outside the lock
      reactor_->post([self = shared_from_this()] { self->schedule_close(); });
      return;
    }
  }
  if (write_armed_) {
    reactor_->modify(fd_, Reactor::Read);
    write_armed_ = false;
  }
}

void Connection::on_writable() {
  std::lock_guard<std::mutex> lk(mu_);
  try_flush_locked();
}

void Connection::close() { schedule_close(); }

void Connection::schedule_close() {
  bool expected = false;
  if (!closing_.compare_exchange_strong(expected, true)) return;
  auto self = shared_from_this();
  reactor_->post([self] { self->deliver_close(); });
}

void Connection::deliver_close() {
  bool expected = false;
  if (!closed_.compare_exchange_strong(expected, true)) return;
  reactor_->remove(fd_);
  if (socket_valid(fd_)) {
    socket_close(fd_);
    fd_ = CHAT_SOCKET_INVALID;
  }
  if (on_close_) {
    try { on_close_(shared_from_this()); } catch (...) {}
  }
}

}  // namespace chat::net
