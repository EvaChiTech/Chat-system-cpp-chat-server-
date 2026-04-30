#pragma once
#include "db/mysql_pool.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace chat::db {

struct User {
  std::uint64_t id;
  std::string   username;
  std::string   password_hash;
  std::int64_t  created_at_ms;
  std::int64_t  last_seen_ms;
};

struct SessionRow {
  std::string   token;
  std::uint64_t user_id;
  std::int64_t  last_active_ms;
};

struct Room {
  std::uint64_t id;
  std::string   name;
  std::int64_t  created_at_ms;
};

struct Message {
  std::uint64_t id;
  std::uint64_t sender_id;
  std::optional<std::uint64_t> receiver_id;
  std::optional<std::uint64_t> room_id;
  std::string   content;
  std::uint8_t  status;
  std::int64_t  created_at_ms;
};

class Repositories {
 public:
  // Pool may be nullptr (CHAT_NO_MYSQL build); in that case all calls go to
  // an in-memory store.
  explicit Repositories(MysqlPool* pool);

  // ---- users ----
  std::optional<User> find_user_by_id(std::uint64_t id);
  std::optional<User> find_user_by_username(const std::string& name);
  // Returns the new user_id, or nullopt if username already exists.
  std::optional<std::uint64_t> create_user(const std::string& username,
                                           const std::string& password_hash);
  void update_last_seen(std::uint64_t user_id, std::int64_t ts_ms);

  // ---- sessions ----
  std::vector<SessionRow> load_all_sessions();
  void insert_session(const std::string& token, std::uint64_t user_id, std::int64_t ts_ms);
  void delete_session(const std::string& token);

  // ---- rooms ----
  std::optional<Room> find_room_by_name(const std::string& name);
  std::optional<Room> find_room_by_id(std::uint64_t id);
  std::optional<std::uint64_t> create_room(const std::string& name);
  bool join_room(std::uint64_t room_id, std::uint64_t user_id);
  bool leave_room(std::uint64_t room_id, std::uint64_t user_id);
  bool is_member(std::uint64_t room_id, std::uint64_t user_id);
  std::vector<Room> list_rooms_for_user(std::uint64_t user_id);
  std::vector<std::uint64_t> list_room_members(std::uint64_t room_id);

  // ---- messages ----
  std::optional<std::uint64_t> insert_dm(std::uint64_t sender,
                                         std::uint64_t receiver,
                                         const std::string& content,
                                         std::int64_t ts_ms);
  std::optional<std::uint64_t> insert_room_msg(std::uint64_t sender,
                                               std::uint64_t room,
                                               const std::string& content,
                                               std::int64_t ts_ms);
  std::vector<Message> history_dm(std::uint64_t a, std::uint64_t b,
                                  std::uint64_t before_id, std::uint32_t limit);
  std::vector<Message> history_room(std::uint64_t room_id,
                                    std::uint64_t before_id,
                                    std::uint32_t limit);
  void update_message_status(std::uint64_t msg_id, std::uint8_t status);

  // ---- undelivered (offline queue) ----
  void enqueue_undelivered(std::uint64_t msg_id, std::uint64_t user_id);
  std::vector<Message> drain_undelivered(std::uint64_t user_id);

 private:
  MysqlPool* pool_;

#if defined(CHAT_NO_MYSQL)
  // In-memory backing store.
  std::mutex mu_;
  std::uint64_t next_user_id_ = 1;
  std::uint64_t next_room_id_ = 1;
  std::uint64_t next_msg_id_  = 1;
  std::unordered_map<std::uint64_t, User> users_;
  std::unordered_map<std::string, std::uint64_t> username_idx_;
  std::vector<SessionRow> sessions_;
  std::unordered_map<std::uint64_t, Room> rooms_;
  std::unordered_map<std::string, std::uint64_t> room_name_idx_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> room_members_;
  std::vector<Message> messages_;
  std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> undelivered_;
#endif
};

}  // namespace chat::db
