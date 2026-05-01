// Repositories — MySQL when CHAT_NO_MYSQL is unset, else an in-memory store.
//
// The MySQL path uses mysql_real_query with mysql_real_escape_string for
// strings; integers are formatted directly. This is safe against SQL injection
// as long as every user-supplied string is run through escape() (we do).
#include "db/repositories.h"
#include "common/logger.h"
#include "common/util.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#if !defined(CHAT_NO_MYSQL)
#include <mysql.h>
#endif

namespace chat::db {

#if !defined(CHAT_NO_MYSQL)

namespace {

// Escape a string for safe use in a single-quoted SQL literal.
std::string escape(MYSQL* c, const std::string& s) {
  std::string out;
  out.resize(s.size() * 2 + 1);
  unsigned long n = ::mysql_real_escape_string(c, out.data(), s.data(),
                                               static_cast<unsigned long>(s.size()));
  out.resize(n);
  return out;
}

// Parse a "YYYY-MM-DD hh:mm:ss[.fff]" timestamp into unix ms.
std::int64_t parse_ts(const char* s) {
  if (!s) return 0;
  std::tm tm{};
  int ms = 0;
  // sscanf is fine here; values are server-controlled.
  if (std::sscanf(s, "%d-%d-%d %d:%d:%d.%d",
                  &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                  &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &ms) < 6) {
    return 0;
  }
  tm.tm_year -= 1900;
  tm.tm_mon  -= 1;
#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = ::timegm(&tm);
#endif
  if (t < 0) return 0;
  return static_cast<std::int64_t>(t) * 1000 + ms;
}

bool exec(MYSQL* c, const std::string& q) {
  if (::mysql_real_query(c, q.data(), static_cast<unsigned long>(q.size())) != 0) {
    LOG_ERROR("mysql query failed: " << ::mysql_error(c) << " | " << q);
    return false;
  }
  return true;
}

}  // namespace

Repositories::Repositories(MysqlPool* p) : pool_(p) {}

// ---- users -----------------------------------------------------------

std::optional<User> Repositories::find_user_by_id(std::uint64_t id) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT id,username,password_hash,UNIX_TIMESTAMP(created_at)*1000,"
    << "COALESCE(UNIX_TIMESTAMP(last_seen_at)*1000,0) FROM users WHERE id="
    << id << " LIMIT 1";
  if (!exec(c, q.str())) return std::nullopt;
  MYSQL_RES* r = ::mysql_store_result(c);
  std::optional<User> out;
  if (r) {
    if (auto* row = ::mysql_fetch_row(r)) {
      out = User{
        std::stoull(row[0] ? row[0] : "0"),
        row[1] ? row[1] : "",
        row[2] ? row[2] : "",
        row[3] ? std::stoll(row[3]) : 0,
        row[4] ? std::stoll(row[4]) : 0,
      };
    }
    ::mysql_free_result(r);
  }
  return out;
}

std::optional<User> Repositories::find_user_by_username(const std::string& name) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT id,username,password_hash,UNIX_TIMESTAMP(created_at)*1000,"
    << "COALESCE(UNIX_TIMESTAMP(last_seen_at)*1000,0) FROM users WHERE username='"
    << escape(c, name) << "' LIMIT 1";
  if (!exec(c, q.str())) return std::nullopt;
  MYSQL_RES* r = ::mysql_store_result(c);
  std::optional<User> out;
  if (r) {
    if (auto* row = ::mysql_fetch_row(r)) {
      out = User{
        std::stoull(row[0] ? row[0] : "0"),
        row[1] ? row[1] : "",
        row[2] ? row[2] : "",
        row[3] ? std::stoll(row[3]) : 0,
        row[4] ? std::stoll(row[4]) : 0,
      };
    }
    ::mysql_free_result(r);
  }
  return out;
}

std::optional<std::uint64_t> Repositories::create_user(const std::string& username,
                                                       const std::string& password_hash) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "INSERT INTO users(username,password_hash) VALUES('"
    << escape(c, username) << "','" << escape(c, password_hash) << "')";
  if (::mysql_real_query(c, q.str().data(),
                         static_cast<unsigned long>(q.str().size())) != 0) {
    // 1062 = ER_DUP_ENTRY
    if (::mysql_errno(c) == 1062) return std::nullopt;
    LOG_ERROR("create_user failed: " << ::mysql_error(c));
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(::mysql_insert_id(c));
}

void Repositories::update_last_seen(std::uint64_t user_id, std::int64_t ts_ms) {
  auto lease = pool_->acquire();
  if (!lease) return;
  auto* c = lease.get();
  std::ostringstream q;
  q << "UPDATE users SET last_seen_at=FROM_UNIXTIME(" << (ts_ms / 1000)
    << ") WHERE id=" << user_id;
  exec(c, q.str());
}

// ---- sessions --------------------------------------------------------

std::vector<SessionRow> Repositories::load_all_sessions() {
  std::vector<SessionRow> out;
  auto lease = pool_->acquire();
  if (!lease) return out;
  auto* c = lease.get();
  if (!exec(c, "SELECT token,user_id,UNIX_TIMESTAMP(last_active_at)*1000 FROM sessions"))
    return out;
  MYSQL_RES* r = ::mysql_store_result(c);
  if (!r) return out;
  while (auto* row = ::mysql_fetch_row(r)) {
    out.push_back(SessionRow{
      row[0] ? row[0] : "",
      row[1] ? std::stoull(row[1]) : 0,
      row[2] ? std::stoll(row[2]) : 0,
    });
  }
  ::mysql_free_result(r);
  return out;
}

void Repositories::insert_session(const std::string& token,
                                  std::uint64_t user_id, std::int64_t ts_ms) {
  auto lease = pool_->acquire();
  if (!lease) return;
  auto* c = lease.get();
  std::ostringstream q;
  q << "INSERT INTO sessions(token,user_id,last_active_at) VALUES('"
    << escape(c, token) << "'," << user_id
    << ",FROM_UNIXTIME(" << (ts_ms / 1000) << "))"
       " ON DUPLICATE KEY UPDATE last_active_at=VALUES(last_active_at)";
  exec(c, q.str());
}

void Repositories::delete_session(const std::string& token) {
  auto lease = pool_->acquire();
  if (!lease) return;
  auto* c = lease.get();
  std::ostringstream q;
  q << "DELETE FROM sessions WHERE token='" << escape(c, token) << "'";
  exec(c, q.str());
}

// ---- rooms -----------------------------------------------------------

std::optional<Room> Repositories::find_room_by_name(const std::string& name) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT id,name,UNIX_TIMESTAMP(created_at)*1000 FROM rooms WHERE name='"
    << escape(c, name) << "' LIMIT 1";
  if (!exec(c, q.str())) return std::nullopt;
  MYSQL_RES* r = ::mysql_store_result(c);
  std::optional<Room> out;
  if (r) {
    if (auto* row = ::mysql_fetch_row(r)) {
      out = Room{
        row[0] ? std::stoull(row[0]) : 0,
        row[1] ? row[1] : "",
        row[2] ? std::stoll(row[2]) : 0
      };
    }
    ::mysql_free_result(r);
  }
  return out;
}

std::optional<Room> Repositories::find_room_by_id(std::uint64_t id) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT id,name,UNIX_TIMESTAMP(created_at)*1000 FROM rooms WHERE id=" << id;
  if (!exec(c, q.str())) return std::nullopt;
  MYSQL_RES* r = ::mysql_store_result(c);
  std::optional<Room> out;
  if (r) {
    if (auto* row = ::mysql_fetch_row(r)) {
      out = Room{
        row[0] ? std::stoull(row[0]) : 0,
        row[1] ? row[1] : "",
        row[2] ? std::stoll(row[2]) : 0
      };
    }
    ::mysql_free_result(r);
  }
  return out;
}

std::optional<std::uint64_t> Repositories::create_room(const std::string& name) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "INSERT INTO rooms(name) VALUES('" << escape(c, name) << "')";
  if (::mysql_real_query(c, q.str().data(),
                         static_cast<unsigned long>(q.str().size())) != 0) {
    if (::mysql_errno(c) == 1062) return std::nullopt;
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(::mysql_insert_id(c));
}

bool Repositories::join_room(std::uint64_t room_id, std::uint64_t user_id) {
  auto lease = pool_->acquire();
  if (!lease) return false;
  auto* c = lease.get();
  std::ostringstream q;
  q << "INSERT IGNORE INTO room_members(room_id,user_id) VALUES("
    << room_id << "," << user_id << ")";
  return exec(c, q.str());
}

bool Repositories::leave_room(std::uint64_t room_id, std::uint64_t user_id) {
  auto lease = pool_->acquire();
  if (!lease) return false;
  auto* c = lease.get();
  std::ostringstream q;
  q << "DELETE FROM room_members WHERE room_id=" << room_id
    << " AND user_id=" << user_id;
  return exec(c, q.str());
}

bool Repositories::is_member(std::uint64_t room_id, std::uint64_t user_id) {
  auto lease = pool_->acquire();
  if (!lease) return false;
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT 1 FROM room_members WHERE room_id=" << room_id
    << " AND user_id=" << user_id << " LIMIT 1";
  if (!exec(c, q.str())) return false;
  MYSQL_RES* r = ::mysql_store_result(c);
  bool yes = r && ::mysql_fetch_row(r);
  if (r) ::mysql_free_result(r);
  return yes;
}

std::vector<Room> Repositories::list_rooms_for_user(std::uint64_t user_id) {
  std::vector<Room> out;
  auto lease = pool_->acquire();
  if (!lease) return out;
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT r.id,r.name,UNIX_TIMESTAMP(r.created_at)*1000 "
       "FROM rooms r INNER JOIN room_members m ON m.room_id=r.id "
       "WHERE m.user_id=" << user_id;
  if (!exec(c, q.str())) return out;
  MYSQL_RES* r = ::mysql_store_result(c);
  if (!r) return out;
  while (auto* row = ::mysql_fetch_row(r)) {
    out.push_back(Room{
      row[0] ? std::stoull(row[0]) : 0,
      row[1] ? row[1] : "",
      row[2] ? std::stoll(row[2]) : 0,
    });
  }
  ::mysql_free_result(r);
  return out;
}

std::vector<std::uint64_t> Repositories::list_room_members(std::uint64_t room_id) {
  std::vector<std::uint64_t> out;
  auto lease = pool_->acquire();
  if (!lease) return out;
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT user_id FROM room_members WHERE room_id=" << room_id;
  if (!exec(c, q.str())) return out;
  MYSQL_RES* r = ::mysql_store_result(c);
  if (!r) return out;
  while (auto* row = ::mysql_fetch_row(r)) {
    if (row[0]) out.push_back(std::stoull(row[0]));
  }
  ::mysql_free_result(r);
  return out;
}

// ---- messages --------------------------------------------------------

std::optional<std::uint64_t> Repositories::insert_dm(std::uint64_t s,
                                                     std::uint64_t r,
                                                     const std::string& content,
                                                     std::int64_t ts) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "INSERT INTO messages(sender_id,receiver_id,content,created_at) VALUES("
    << s << "," << r << ",'" << escape(c, content)
    << "',FROM_UNIXTIME(" << (ts / 1000) << "."
    << (ts % 1000) << "))";
  if (::mysql_real_query(c, q.str().data(),
                         static_cast<unsigned long>(q.str().size())) != 0)
    return std::nullopt;
  return static_cast<std::uint64_t>(::mysql_insert_id(c));
}

std::optional<std::uint64_t> Repositories::insert_room_msg(std::uint64_t s,
                                                           std::uint64_t room,
                                                           const std::string& content,
                                                           std::int64_t ts) {
  auto lease = pool_->acquire();
  if (!lease) return std::nullopt;
  auto* c = lease.get();
  std::ostringstream q;
  q << "INSERT INTO messages(sender_id,room_id,content,created_at) VALUES("
    << s << "," << room << ",'" << escape(c, content)
    << "',FROM_UNIXTIME(" << (ts / 1000) << "."
    << (ts % 1000) << "))";
  if (::mysql_real_query(c, q.str().data(),
                         static_cast<unsigned long>(q.str().size())) != 0)
    return std::nullopt;
  return static_cast<std::uint64_t>(::mysql_insert_id(c));
}

static std::vector<Message> read_messages(MYSQL_RES* r) {
  std::vector<Message> out;
  if (!r) return out;
  while (auto* row = ::mysql_fetch_row(r)) {
    Message m{};
    m.id            = row[0] ? std::stoull(row[0]) : 0;
    m.sender_id     = row[1] ? std::stoull(row[1]) : 0;
    if (row[2]) m.receiver_id = std::stoull(row[2]);
    if (row[3]) m.room_id     = std::stoull(row[3]);
    m.content       = row[4] ? row[4] : "";
    m.status        = row[5] ? static_cast<std::uint8_t>(std::stoi(row[5])) : 0;
    m.created_at_ms = row[6] ? std::stoll(row[6]) : 0;
    out.push_back(std::move(m));
  }
  return out;
}

std::vector<Message> Repositories::history_dm(std::uint64_t a, std::uint64_t b,
                                              std::uint64_t before, std::uint32_t lim) {
  auto lease = pool_->acquire();
  if (!lease) return {};
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT id,sender_id,receiver_id,room_id,content,status,"
       "UNIX_TIMESTAMP(created_at)*1000 FROM messages "
       "WHERE ((sender_id=" << a << " AND receiver_id=" << b << ")"
       " OR (sender_id=" << b << " AND receiver_id=" << a << "))";
  if (before) q << " AND id<" << before;
  q << " ORDER BY id DESC LIMIT " << std::min<std::uint32_t>(lim, 200);
  if (!exec(c, q.str())) return {};
  MYSQL_RES* r = ::mysql_store_result(c);
  auto out = read_messages(r);
  if (r) ::mysql_free_result(r);
  std::reverse(out.begin(), out.end());  // oldest first for the client
  return out;
}

std::vector<Message> Repositories::history_room(std::uint64_t room_id,
                                                std::uint64_t before,
                                                std::uint32_t lim) {
  auto lease = pool_->acquire();
  if (!lease) return {};
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT id,sender_id,receiver_id,room_id,content,status,"
       "UNIX_TIMESTAMP(created_at)*1000 FROM messages WHERE room_id=" << room_id;
  if (before) q << " AND id<" << before;
  q << " ORDER BY id DESC LIMIT " << std::min<std::uint32_t>(lim, 200);
  if (!exec(c, q.str())) return {};
  MYSQL_RES* r = ::mysql_store_result(c);
  auto out = read_messages(r);
  if (r) ::mysql_free_result(r);
  std::reverse(out.begin(), out.end());
  return out;
}

void Repositories::update_message_status(std::uint64_t msg_id, std::uint8_t status) {
  auto lease = pool_->acquire();
  if (!lease) return;
  auto* c = lease.get();
  std::ostringstream q;
  q << "UPDATE messages SET status=" << static_cast<int>(status)
    << " WHERE id=" << msg_id;
  exec(c, q.str());
}

// ---- undelivered ----------------------------------------------------

void Repositories::enqueue_undelivered(std::uint64_t msg_id, std::uint64_t user_id) {
  auto lease = pool_->acquire();
  if (!lease) return;
  auto* c = lease.get();
  std::ostringstream q;
  q << "INSERT IGNORE INTO undelivered(message_id,user_id) VALUES("
    << msg_id << "," << user_id << ")";
  exec(c, q.str());
}

std::vector<Message> Repositories::drain_undelivered(std::uint64_t user_id) {
  auto lease = pool_->acquire();
  if (!lease) return {};
  auto* c = lease.get();
  std::ostringstream q;
  q << "SELECT m.id,m.sender_id,m.receiver_id,m.room_id,m.content,m.status,"
       "UNIX_TIMESTAMP(m.created_at)*1000 "
       "FROM messages m INNER JOIN undelivered u ON u.message_id=m.id "
       "WHERE u.user_id=" << user_id << " ORDER BY m.id ASC";
  if (!exec(c, q.str())) return {};
  MYSQL_RES* r = ::mysql_store_result(c);
  auto out = read_messages(r);
  if (r) ::mysql_free_result(r);

  std::ostringstream del;
  del << "DELETE FROM undelivered WHERE user_id=" << user_id;
  exec(c, del.str());
  return out;
}

#else  // CHAT_NO_MYSQL — in-memory store -----------------------------

Repositories::Repositories(MysqlPool*) : pool_(nullptr) {}

std::optional<User> Repositories::find_user_by_id(std::uint64_t id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = users_.find(id);
  return it == users_.end() ? std::nullopt : std::optional<User>(it->second);
}

std::optional<User> Repositories::find_user_by_username(const std::string& n) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = username_idx_.find(n);
  if (it == username_idx_.end()) return std::nullopt;
  return users_.at(it->second);
}

std::optional<std::uint64_t> Repositories::create_user(const std::string& n,
                                                       const std::string& h) {
  std::lock_guard<std::mutex> lk(mu_);
  if (username_idx_.count(n)) return std::nullopt;
  auto id = next_user_id_++;
  users_[id] = User{id, n, h, util::now_unix_ms(), 0};
  username_idx_[n] = id;
  return id;
}

void Repositories::update_last_seen(std::uint64_t id, std::int64_t ts) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = users_.find(id);
  if (it != users_.end()) it->second.last_seen_ms = ts;
}

std::vector<SessionRow> Repositories::load_all_sessions() {
  std::lock_guard<std::mutex> lk(mu_);
  return sessions_;
}

void Repositories::insert_session(const std::string& tok, std::uint64_t uid,
                                  std::int64_t ts) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& s : sessions_) {
    if (s.token == tok) { s.user_id = uid; s.last_active_ms = ts; return; }
  }
  sessions_.push_back(SessionRow{tok, uid, ts});
}

void Repositories::delete_session(const std::string& tok) {
  std::lock_guard<std::mutex> lk(mu_);
  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                 [&](const SessionRow& r){ return r.token == tok; }),
                  sessions_.end());
}

std::optional<Room> Repositories::find_room_by_name(const std::string& n) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = room_name_idx_.find(n);
  if (it == room_name_idx_.end()) return std::nullopt;
  return rooms_.at(it->second);
}

std::optional<Room> Repositories::find_room_by_id(std::uint64_t id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = rooms_.find(id);
  return it == rooms_.end() ? std::nullopt : std::optional<Room>(it->second);
}

std::optional<std::uint64_t> Repositories::create_room(const std::string& n) {
  std::lock_guard<std::mutex> lk(mu_);
  if (room_name_idx_.count(n)) return std::nullopt;
  auto id = next_room_id_++;
  rooms_[id] = Room{id, n, util::now_unix_ms()};
  room_name_idx_[n] = id;
  return id;
}

bool Repositories::join_room(std::uint64_t r, std::uint64_t u) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!rooms_.count(r)) return false;
  room_members_[r].insert(u);
  return true;
}

bool Repositories::leave_room(std::uint64_t r, std::uint64_t u) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = room_members_.find(r);
  if (it == room_members_.end()) return false;
  it->second.erase(u);
  return true;
}

bool Repositories::is_member(std::uint64_t r, std::uint64_t u) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = room_members_.find(r);
  return it != room_members_.end() && it->second.count(u) > 0;
}

std::vector<Room> Repositories::list_rooms_for_user(std::uint64_t u) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Room> out;
  for (auto& [rid, members] : room_members_) {
    if (members.count(u)) out.push_back(rooms_.at(rid));
  }
  return out;
}

std::vector<std::uint64_t> Repositories::list_room_members(std::uint64_t r) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::uint64_t> out;
  auto it = room_members_.find(r);
  if (it == room_members_.end()) return out;
  out.assign(it->second.begin(), it->second.end());
  return out;
}

std::optional<std::uint64_t> Repositories::insert_dm(std::uint64_t s,
                                                     std::uint64_t r,
                                                     const std::string& c,
                                                     std::int64_t ts) {
  std::lock_guard<std::mutex> lk(mu_);
  auto id = next_msg_id_++;
  Message m{}; m.id = id; m.sender_id = s; m.receiver_id = r;
  m.content = c; m.status = 0; m.created_at_ms = ts;
  messages_.push_back(std::move(m));
  return id;
}

std::optional<std::uint64_t> Repositories::insert_room_msg(std::uint64_t s,
                                                           std::uint64_t room,
                                                           const std::string& c,
                                                           std::int64_t ts) {
  std::lock_guard<std::mutex> lk(mu_);
  auto id = next_msg_id_++;
  Message m{}; m.id = id; m.sender_id = s; m.room_id = room;
  m.content = c; m.status = 0; m.created_at_ms = ts;
  messages_.push_back(std::move(m));
  return id;
}

std::vector<Message> Repositories::history_dm(std::uint64_t a, std::uint64_t b,
                                              std::uint64_t before, std::uint32_t lim) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Message> out;
  for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
    if (out.size() >= lim) break;
    if (before && it->id >= before) continue;
    bool match = it->receiver_id &&
                 ((it->sender_id == a && *it->receiver_id == b) ||
                  (it->sender_id == b && *it->receiver_id == a));
    if (match) out.push_back(*it);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<Message> Repositories::history_room(std::uint64_t r,
                                                std::uint64_t before,
                                                std::uint32_t lim) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Message> out;
  for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
    if (out.size() >= lim) break;
    if (before && it->id >= before) continue;
    if (it->room_id && *it->room_id == r) out.push_back(*it);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

void Repositories::update_message_status(std::uint64_t id, std::uint8_t st) {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& m : messages_) if (m.id == id) { m.status = st; return; }
}

void Repositories::enqueue_undelivered(std::uint64_t mid, std::uint64_t uid) {
  std::lock_guard<std::mutex> lk(mu_);
  undelivered_[uid].push_back(mid);
}

std::vector<Message> Repositories::drain_undelivered(std::uint64_t uid) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Message> out;
  auto it = undelivered_.find(uid);
  if (it == undelivered_.end()) return out;
  for (auto mid : it->second) {
    for (auto& m : messages_) if (m.id == mid) { out.push_back(m); break; }
  }
  undelivered_.erase(it);
  return out;
}

#endif  // CHAT_NO_MYSQL

}  // namespace chat::db
