#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chat::db { class Repositories; }

namespace chat {

// In-memory cache of room membership backed by the DB. The DB remains the
// source of truth; this cache is kept warm for the fan-out hot path.
class RoomManager {
 public:
  explicit RoomManager(chat::db::Repositories* repos);

  // Hot-load membership for any room a user is in. Lazy: loads on first use.
  std::vector<std::uint64_t> members_of(std::uint64_t room_id);
  bool is_member(std::uint64_t room_id, std::uint64_t user_id);

  // After DB write succeeds, update the cache.
  void on_join(std::uint64_t room_id, std::uint64_t user_id);
  void on_leave(std::uint64_t room_id, std::uint64_t user_id);
  void invalidate(std::uint64_t room_id);

 private:
  void ensure_loaded(std::uint64_t room_id);

  chat::db::Repositories* repos_;
  std::mutex mu_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> cache_;
  std::unordered_set<std::uint64_t> loaded_;
};

}  // namespace chat
