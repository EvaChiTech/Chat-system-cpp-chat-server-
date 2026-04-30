#include "chat/room_manager.h"
#include "db/repositories.h"

namespace chat {

RoomManager::RoomManager(chat::db::Repositories* r) : repos_(r) {}

void RoomManager::ensure_loaded(std::uint64_t room_id) {
  if (loaded_.count(room_id)) return;
  auto members = repos_->list_room_members(room_id);
  cache_[room_id] = std::unordered_set<std::uint64_t>(members.begin(), members.end());
  loaded_.insert(room_id);
}

std::vector<std::uint64_t> RoomManager::members_of(std::uint64_t room_id) {
  std::lock_guard<std::mutex> lk(mu_);
  ensure_loaded(room_id);
  auto it = cache_.find(room_id);
  if (it == cache_.end()) return {};
  return std::vector<std::uint64_t>(it->second.begin(), it->second.end());
}

bool RoomManager::is_member(std::uint64_t room_id, std::uint64_t user_id) {
  std::lock_guard<std::mutex> lk(mu_);
  ensure_loaded(room_id);
  auto it = cache_.find(room_id);
  return it != cache_.end() && it->second.count(user_id);
}

void RoomManager::on_join(std::uint64_t room_id, std::uint64_t user_id) {
  std::lock_guard<std::mutex> lk(mu_);
  ensure_loaded(room_id);
  cache_[room_id].insert(user_id);
}

void RoomManager::on_leave(std::uint64_t room_id, std::uint64_t user_id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cache_.find(room_id);
  if (it != cache_.end()) it->second.erase(user_id);
}

void RoomManager::invalidate(std::uint64_t room_id) {
  std::lock_guard<std::mutex> lk(mu_);
  cache_.erase(room_id);
  loaded_.erase(room_id);
}

}  // namespace chat
