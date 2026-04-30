#include "protocol/messages.h"

namespace chat::protocol {

json make_ack(std::optional<std::int64_t> id, json data) {
  json j = std::move(data);
  j["type"] = types::kAck;
  if (id) j["id"] = *id;
  return j;
}

json make_error(std::optional<std::int64_t> id, const std::string& code,
                const std::string& message) {
  json j;
  j["type"]    = types::kError;
  j["code"]    = code;
  j["message"] = message;
  if (id) j["id"] = *id;
  return j;
}

json make_event_message(std::int64_t msg_id, std::int64_t sender_id,
                        std::optional<std::int64_t> receiver_id,
                        std::optional<std::int64_t> room_id,
                        const std::string& content,
                        std::int64_t created_at_ms) {
  json j;
  j["type"]      = types::kEventMessage;
  j["msg_id"]    = msg_id;
  j["sender_id"] = sender_id;
  if (receiver_id) j["receiver_id"] = *receiver_id;
  if (room_id)     j["room_id"]     = *room_id;
  j["content"]   = content;
  j["timestamp"] = created_at_ms;
  return j;
}

json make_event_presence(std::int64_t user_id, const std::string& username,
                         bool online, std::int64_t last_seen_ms) {
  json j;
  j["type"]      = types::kEventPresence;
  j["user_id"]   = user_id;
  j["username"]  = username;
  j["online"]    = online;
  j["last_seen"] = last_seen_ms;
  return j;
}

std::optional<json> parse(const std::string& s) {
  try {
    return json::parse(s);
  } catch (...) {
    return std::nullopt;
  }
}

std::string serialize(const json& j) {
  return j.dump();
}

std::string get_str(const json& j, const char* k, const std::string& def) {
  auto it = j.find(k);
  if (it == j.end() || !it->is_string()) return def;
  return it->get<std::string>();
}

std::int64_t get_i64(const json& j, const char* k, std::int64_t def) {
  auto it = j.find(k);
  if (it == j.end()) return def;
  if (it->is_number_integer()) return it->get<std::int64_t>();
  if (it->is_number_unsigned()) return static_cast<std::int64_t>(it->get<std::uint64_t>());
  if (it->is_number_float())   return static_cast<std::int64_t>(it->get<double>());
  if (it->is_string()) {
    try { return std::stoll(it->get<std::string>()); } catch (...) {}
  }
  return def;
}

}  // namespace chat::protocol
