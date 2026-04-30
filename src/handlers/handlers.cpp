#include "handlers/handlers.h"
#include "chat/chat_server.h"
#include "auth/crypto.h"
#include "common/logger.h"
#include "common/util.h"
#include "protocol/messages.h"

#include <optional>
#include <string>

namespace chat::handlers {

using namespace chat::protocol;

namespace {

void send(net::Connection* c, const json& j) {
  c->send_frame(j.dump());
}

// Returns the user_id bound to this connection, or 0 if unauth'd.
// Validates the session token if present in the request.
std::uint64_t require_auth(ChatServer* s, net::Connection* c, const json& req,
                            std::optional<std::int64_t> id) {
  // Already-authenticated connections carry the user_id directly.
  if (auto uid = c->user_id(); uid != 0) return uid;
  // Fall back to a token in the request (some clients re-auth per request).
  std::string tok = get_str(req, "token");
  if (tok.empty()) {
    send(c, make_error(id, errcode::kUnauthorized, "missing session token"));
    return 0;
  }
  auto info = s->sessions()->validate(tok);
  if (!info) {
    send(c, make_error(id, errcode::kUnauthorized, "invalid or expired token"));
    return 0;
  }
  c->set_user_id(info->user_id);
  c->set_session_token(tok);
  s->presence()->bind(info->user_id, c->shared_from_this());
  return info->user_id;
}

// ---- per-handler implementations ----

void handle_register(ChatServer* s, net::Connection* c, const json& req,
                     std::optional<std::int64_t> id) {
  auto user = get_str(req, "username");
  auto pass = get_str(req, "password");
  if (user.empty() || pass.empty() || user.size() > 64 || pass.size() < 4) {
    send(c, make_error(id, errcode::kBadRequest, "username/password invalid"));
    return;
  }
  auto hash = chat::auth::make_password_hash(pass, s->pbkdf2_iter());
  auto new_id = s->repos()->create_user(user, hash);
  if (!new_id) {
    send(c, make_error(id, errcode::kConflict, "username already taken"));
    return;
  }
  json data;
  data["user_id"] = *new_id;
  data["username"] = user;
  send(c, make_ack(id, data));
}

void handle_login(ChatServer* s, net::Connection* c, const json& req,
                  std::optional<std::int64_t> id) {
  auto user = get_str(req, "username");
  auto pass = get_str(req, "password");
  auto u = s->repos()->find_user_by_username(user);
  if (!u || !chat::auth::verify_password_hash(pass, u->password_hash)) {
    send(c, make_error(id, errcode::kUnauthorized, "bad credentials"));
    return;
  }
  auto token = s->sessions()->create(u->id);
  c->set_user_id(u->id);
  c->set_session_token(token);
  s->presence()->bind(u->id, c->shared_from_this());

  json data;
  data["user_id"]  = u->id;
  data["username"] = u->username;
  data["token"]    = token;
  data["ttl_s"]    = s->sessions()->ttl_seconds();
  send(c, make_ack(id, data));
}

void handle_logout(ChatServer* s, net::Connection* c, const json& /*req*/,
                   std::optional<std::int64_t> id) {
  auto tok = c->session_token();
  if (!tok.empty()) s->sessions()->revoke(tok);
  if (auto uid = c->user_id(); uid) {
    s->presence()->unbind(uid, c->shared_from_this());
  }
  c->set_user_id(0);
  c->set_session_token("");
  send(c, make_ack(id));
}

void handle_send_dm(ChatServer* s, net::Connection* c, const json& req,
                    std::optional<std::int64_t> id) {
  auto sender = require_auth(s, c, req, id);
  if (!sender) return;
  auto receiver = static_cast<std::uint64_t>(get_i64(req, "receiver_id"));
  auto content  = get_str(req, "content");
  if (!receiver || content.empty()) {
    send(c, make_error(id, errcode::kBadRequest, "receiver_id and content required"));
    return;
  }
  if (content.size() > s->max_message()) {
    send(c, make_error(id, errcode::kBadRequest, "content too large"));
    return;
  }
  if (!s->repos()->find_user_by_id(receiver)) {
    send(c, make_error(id, errcode::kNotFound, "receiver does not exist"));
    return;
  }
  auto msg_id = s->router()->send_dm(sender, receiver, content);
  if (!msg_id) {
    send(c, make_error(id, errcode::kInternal, "failed to persist message"));
    return;
  }
  json d; d["msg_id"] = msg_id; d["timestamp"] = util::now_unix_ms();
  send(c, make_ack(id, d));
}

void handle_send_room(ChatServer* s, net::Connection* c, const json& req,
                      std::optional<std::int64_t> id) {
  auto sender = require_auth(s, c, req, id);
  if (!sender) return;
  auto room    = static_cast<std::uint64_t>(get_i64(req, "room_id"));
  auto content = get_str(req, "content");
  if (!room || content.empty()) {
    send(c, make_error(id, errcode::kBadRequest, "room_id and content required"));
    return;
  }
  if (content.size() > s->max_message()) {
    send(c, make_error(id, errcode::kBadRequest, "content too large"));
    return;
  }
  auto msg_id = s->router()->send_room(sender, room, content);
  if (!msg_id) {
    send(c, make_error(id, errcode::kBadRequest, "not a member of room or room missing"));
    return;
  }
  json d; d["msg_id"] = msg_id; d["timestamp"] = util::now_unix_ms();
  send(c, make_ack(id, d));
}

void handle_create_room(ChatServer* s, net::Connection* c, const json& req,
                        std::optional<std::int64_t> id) {
  auto uid = require_auth(s, c, req, id);
  if (!uid) return;
  auto name = get_str(req, "name");
  if (name.empty() || name.size() > 128) {
    send(c, make_error(id, errcode::kBadRequest, "invalid name"));
    return;
  }
  auto rid = s->repos()->create_room(name);
  if (!rid) {
    // Race-safe: if it exists, return it.
    auto existing = s->repos()->find_room_by_name(name);
    if (existing) rid = existing->id;
    else { send(c, make_error(id, errcode::kInternal, "create_room failed")); return; }
  }
  s->repos()->join_room(*rid, uid);
  s->rooms()->on_join(*rid, uid);
  json d; d["room_id"] = *rid; d["name"] = name;
  send(c, make_ack(id, d));
}

void handle_join_room(ChatServer* s, net::Connection* c, const json& req,
                      std::optional<std::int64_t> id) {
  auto uid = require_auth(s, c, req, id);
  if (!uid) return;
  auto rid = static_cast<std::uint64_t>(get_i64(req, "room_id"));
  auto name = get_str(req, "name");
  if (!rid && !name.empty()) {
    auto r = s->repos()->find_room_by_name(name);
    if (r) rid = r->id;
  }
  if (!rid) { send(c, make_error(id, errcode::kNotFound, "room not found")); return; }
  if (!s->repos()->join_room(rid, uid)) {
    send(c, make_error(id, errcode::kInternal, "join failed"));
    return;
  }
  s->rooms()->on_join(rid, uid);
  json d; d["room_id"] = rid;
  send(c, make_ack(id, d));
}

void handle_leave_room(ChatServer* s, net::Connection* c, const json& req,
                       std::optional<std::int64_t> id) {
  auto uid = require_auth(s, c, req, id);
  if (!uid) return;
  auto rid = static_cast<std::uint64_t>(get_i64(req, "room_id"));
  if (!rid) { send(c, make_error(id, errcode::kBadRequest, "room_id required")); return; }
  s->repos()->leave_room(rid, uid);
  s->rooms()->on_leave(rid, uid);
  send(c, make_ack(id));
}

void handle_list_rooms(ChatServer* s, net::Connection* c, const json& req,
                       std::optional<std::int64_t> id) {
  auto uid = require_auth(s, c, req, id);
  if (!uid) return;
  auto rooms = s->repos()->list_rooms_for_user(uid);
  json arr = json::array();
  for (auto& r : rooms) {
    arr.push_back({{"id", r.id}, {"name", r.name}, {"created_at", r.created_at_ms}});
  }
  json d; d["rooms"] = std::move(arr);
  send(c, make_ack(id, d));
}

void handle_history_dm(ChatServer* s, net::Connection* c, const json& req,
                       std::optional<std::int64_t> id) {
  auto uid = require_auth(s, c, req, id);
  if (!uid) return;
  auto other  = static_cast<std::uint64_t>(get_i64(req, "user_id"));
  auto before = static_cast<std::uint64_t>(get_i64(req, "before_id"));
  auto limit  = static_cast<std::uint32_t>(get_i64(req, "limit", 50));
  if (!other) { send(c, make_error(id, errcode::kBadRequest, "user_id required")); return; }
  auto msgs = s->repos()->history_dm(uid, other, before, limit);
  json arr = json::array();
  for (auto& m : msgs) {
    json o;
    o["id"]         = m.id;
    o["sender_id"]  = m.sender_id;
    if (m.receiver_id) o["receiver_id"] = *m.receiver_id;
    o["content"]    = m.content;
    o["status"]     = m.status;
    o["timestamp"]  = m.created_at_ms;
    arr.push_back(std::move(o));
  }
  json d; d["messages"] = std::move(arr);
  send(c, make_ack(id, d));
}

void handle_history_room(ChatServer* s, net::Connection* c, const json& req,
                         std::optional<std::int64_t> id) {
  auto uid = require_auth(s, c, req, id);
  if (!uid) return;
  auto room   = static_cast<std::uint64_t>(get_i64(req, "room_id"));
  auto before = static_cast<std::uint64_t>(get_i64(req, "before_id"));
  auto limit  = static_cast<std::uint32_t>(get_i64(req, "limit", 50));
  if (!room) { send(c, make_error(id, errcode::kBadRequest, "room_id required")); return; }
  if (!s->rooms()->is_member(room, uid)) {
    send(c, make_error(id, errcode::kUnauthorized, "not a member of this room"));
    return;
  }
  auto msgs = s->repos()->history_room(room, before, limit);
  json arr = json::array();
  for (auto& m : msgs) {
    json o;
    o["id"]         = m.id;
    o["sender_id"]  = m.sender_id;
    if (m.room_id) o["room_id"] = *m.room_id;
    o["content"]    = m.content;
    o["status"]     = m.status;
    o["timestamp"]  = m.created_at_ms;
    arr.push_back(std::move(o));
  }
  json d; d["messages"] = std::move(arr);
  send(c, make_ack(id, d));
}

void handle_presence(ChatServer* s, net::Connection* c, const json& req,
                     std::optional<std::int64_t> id) {
  auto uid = require_auth(s, c, req, id);
  if (!uid) return;
  auto target = static_cast<std::uint64_t>(get_i64(req, "user_id"));
  if (!target) { send(c, make_error(id, errcode::kBadRequest, "user_id required")); return; }
  auto u = s->repos()->find_user_by_id(target);
  if (!u) { send(c, make_error(id, errcode::kNotFound, "user not found")); return; }
  json d;
  d["user_id"]   = u->id;
  d["username"]  = u->username;
  d["online"]    = s->presence()->is_online(u->id);
  d["last_seen"] = u->last_seen_ms;
  send(c, make_ack(id, d));
}

void handle_ping(ChatServer* /*s*/, net::Connection* c, const json& /*req*/,
                 std::optional<std::int64_t> id) {
  json d; d["t"] = util::now_unix_ms();
  json r = make_ack(id, d);
  r["type"] = types::kPong;
  send(c, r);
}

}  // anonymous namespace

void dispatch(ChatServer* s, std::shared_ptr<net::Connection> c,
              const std::string& payload) {
  auto parsed = parse(payload);
  if (!parsed) {
    send(c.get(), make_error(std::nullopt, errcode::kBadRequest, "malformed JSON"));
    return;
  }
  json& req = *parsed;

  std::optional<std::int64_t> id;
  if (auto it = req.find("id"); it != req.end() && it->is_number_integer()) {
    id = it->get<std::int64_t>();
  }
  std::string type = get_str(req, "type");
  if (type.empty()) {
    send(c.get(), make_error(id, errcode::kBadRequest, "missing 'type'"));
    return;
  }

  using Fn = void(*)(ChatServer*, net::Connection*, const json&,
                     std::optional<std::int64_t>);
  static const std::unordered_map<std::string, Fn> table = {
    {types::kRegister,    handle_register},
    {types::kLogin,       handle_login},
    {types::kLogout,      handle_logout},
    {types::kSendDM,      handle_send_dm},
    {types::kSendRoom,    handle_send_room},
    {types::kCreateRoom,  handle_create_room},
    {types::kJoinRoom,    handle_join_room},
    {types::kLeaveRoom,   handle_leave_room},
    {types::kListRooms,   handle_list_rooms},
    {types::kHistoryDM,   handle_history_dm},
    {types::kHistoryRoom, handle_history_room},
    {types::kPresence,    handle_presence},
    {types::kPing,        handle_ping},
  };
  auto it = table.find(type);
  if (it == table.end()) {
    send(c.get(), make_error(id, errcode::kBadRequest, "unknown type: " + type));
    return;
  }
  try {
    it->second(s, c.get(), req, id);
  } catch (const std::exception& e) {
    LOG_ERROR("handler '" << type << "' threw: " << e.what());
    send(c.get(), make_error(id, errcode::kInternal, "internal error"));
  }
}

}  // namespace chat::handlers
