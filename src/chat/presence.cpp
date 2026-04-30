#include "chat/presence.h"

namespace chat {

bool Presence::bind(std::uint64_t uid, std::shared_ptr<net::Connection> c) {
  bool transitioned = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto& slot = map_[uid];
    if (slot.empty()) transitioned = true;
    slot[c->id()] = std::move(c);
  }
  if (transitioned && handler_) handler_(uid, true);
  return transitioned;
}

bool Presence::unbind(std::uint64_t uid, std::shared_ptr<net::Connection> c) {
  bool went_offline = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(uid);
    if (it == map_.end()) return false;
    it->second.erase(c->id());
    if (it->second.empty()) {
      map_.erase(it);
      went_offline = true;
    }
  }
  if (went_offline && handler_) handler_(uid, false);
  return went_offline;
}

std::vector<std::shared_ptr<net::Connection>>
Presence::connections_of(std::uint64_t uid) {
  std::vector<std::shared_ptr<net::Connection>> out;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(uid);
  if (it == map_.end()) return out;
  out.reserve(it->second.size());
  for (auto& [_, c] : it->second) out.push_back(c);
  return out;
}

bool Presence::is_online(std::uint64_t uid) {
  std::lock_guard<std::mutex> lk(mu_);
  return map_.count(uid) > 0;
}

}  // namespace chat
