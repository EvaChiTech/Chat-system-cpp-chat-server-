#include "net/server.h"
#include "common/logger.h"

namespace chat::net {

Server::Server(Reactor* r, std::uint32_t max_frame)
    : reactor_(r), max_frame_(max_frame) {}

Server::~Server() { stop(); }

bool Server::listen(const std::string& host, std::uint16_t port) {
  listener_ = Socket::listen_tcp(host, port);
  if (!listener_.valid()) return false;
  socket_t fd = listener_.get();
  reactor_->add(fd, Reactor::Read, [this](int ev) { on_listen_event(ev); });
  LOG_INFO("server listening on " << host << ":" << port);
  return true;
}

void Server::set_handlers(ConnectHandler oc, FrameHandler of, CloseHandler ocl) {
  on_connect_ = std::move(oc);
  on_frame_   = std::move(of);
  on_close_   = std::move(ocl);
}

void Server::stop() {
  if (listener_.valid()) {
    reactor_->remove(listener_.get());
    listener_.close();
  }
  std::vector<std::shared_ptr<Connection>> snap;
  {
    std::lock_guard<std::mutex> lk(mu_);
    snap.reserve(conns_.size());
    for (auto& [_, c] : conns_) snap.push_back(c);
    conns_.clear();
  }
  for (auto& c : snap) c->close();
}

std::size_t Server::connection_count() const {
  std::lock_guard<std::mutex> lk(mu_);
  return conns_.size();
}

void Server::on_listen_event(int /*events*/) {
  for (;;) {
    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    socket_t s = ::accept(listener_.get(),
                          reinterpret_cast<sockaddr*>(&addr), &alen);
    if (!socket_valid(s)) {
      int e = socket_errno();
      if (e == CHAT_EAGAIN || e == CHAT_EINTR) return;
      LOG_WARN("accept errno=" << e);
      return;
    }
    auto c = Connection::make(s, reactor_, max_frame_);
    LOG_DEBUG("conn[" << c->id() << "] accepted fd=" << s);
    register_connection(c);
    if (on_connect_) {
      try { on_connect_(c); } catch (...) {}
    }
    auto self = this;
    c->start(
        [self](std::shared_ptr<Connection> cc, std::string payload) {
          if (self->on_frame_) self->on_frame_(std::move(cc), std::move(payload));
        },
        [self](std::shared_ptr<Connection> cc) {
          if (self->on_close_) self->on_close_(cc);
          self->unregister_connection(std::move(cc));
        });
  }
}

void Server::register_connection(std::shared_ptr<Connection> c) {
  std::lock_guard<std::mutex> lk(mu_);
  conns_.emplace(c->id(), c);
}

void Server::unregister_connection(std::shared_ptr<Connection> c) {
  std::lock_guard<std::mutex> lk(mu_);
  conns_.erase(c->id());
}

}  // namespace chat::net
