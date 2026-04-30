#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>

namespace chat::db { class Repositories; }

namespace chat::auth {

struct SessionInfo {
  std::uint64_t user_id  = 0;
  std::int64_t  expires_at_ms = 0;
};

// In-memory cache of active sessions, write-through to the DB.
// Lookup is O(1) hash; expiry is checked on access (no background thread).
class SessionManager {
 public:
  SessionManager(chat::db::Repositories* repos, std::int64_t ttl_seconds);

  // Hot-load all non-expired sessions from DB. Call once on startup.
  void warm();

  // Creates a new session, persists it, returns the token.
  std::string create(std::uint64_t user_id);
  std::optional<SessionInfo> validate(const std::string& token);
  void touch(const std::string& token);
  void revoke(const std::string& token);

  std::int64_t ttl_seconds() const { return ttl_seconds_; }

 private:
  chat::db::Repositories* repos_;
  std::int64_t            ttl_seconds_;

  mutable std::mutex      mu_;
  std::unordered_map<std::string, SessionInfo> cache_;
};

}  // namespace chat::auth
