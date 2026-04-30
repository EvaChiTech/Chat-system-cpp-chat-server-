#include "auth/session.h"
#include "auth/crypto.h"
#include "db/repositories.h"
#include "common/util.h"
#include "common/logger.h"

namespace chat::auth {

SessionManager::SessionManager(chat::db::Repositories* r, std::int64_t ttl)
    : repos_(r), ttl_seconds_(ttl > 0 ? ttl : 86400) {}

void SessionManager::warm() {
  if (!repos_) return;
  auto now_ms = util::now_unix_ms();
  auto rows = repos_->load_all_sessions();
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& r : rows) {
    auto exp = r.last_active_ms + ttl_seconds_ * 1000;
    if (exp <= now_ms) continue;
    cache_[r.token] = SessionInfo{r.user_id, exp};
  }
  LOG_INFO("session: warmed " << cache_.size() << " active sessions");
}

std::string SessionManager::create(std::uint64_t user_id) {
  auto token = make_session_token();
  auto now_ms = util::now_unix_ms();
  auto exp_ms = now_ms + ttl_seconds_ * 1000;
  {
    std::lock_guard<std::mutex> lk(mu_);
    cache_[token] = SessionInfo{user_id, exp_ms};
  }
  if (repos_) repos_->insert_session(token, user_id, now_ms);
  return token;
}

std::optional<SessionInfo> SessionManager::validate(const std::string& token) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cache_.find(token);
  if (it == cache_.end()) return std::nullopt;
  if (util::now_unix_ms() >= it->second.expires_at_ms) {
    cache_.erase(it);
    return std::nullopt;
  }
  return it->second;
}

void SessionManager::touch(const std::string& token) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = cache_.find(token);
  if (it == cache_.end()) return;
  it->second.expires_at_ms = util::now_unix_ms() + ttl_seconds_ * 1000;
  // We don't write to DB on every touch — periodic flush is cheaper and the
  // worst case is that a TTL is delayed by one period.
}

void SessionManager::revoke(const std::string& token) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    cache_.erase(token);
  }
  if (repos_) repos_->delete_session(token);
}

}  // namespace chat::auth
