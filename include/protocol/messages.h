#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace chat::protocol {

using json = nlohmann::json;

// Every request and response is a JSON object with at minimum:
//   { "type": "<kind>", "id": <opt int>, ...payload }
// id, when set on a request, is echoed in the corresponding response. Server
// pushes (events) have no id field.

namespace types {
constexpr const char* kRegister     = "register";
constexpr const char* kLogin        = "login";
constexpr const char* kLogout       = "logout";
constexpr const char* kSendDM       = "send_dm";
constexpr const char* kSendRoom     = "send_room";
constexpr const char* kCreateRoom   = "create_room";
constexpr const char* kJoinRoom     = "join_room";
constexpr const char* kLeaveRoom    = "leave_room";
constexpr const char* kListRooms    = "list_rooms";
constexpr const char* kHistoryDM    = "history_dm";
constexpr const char* kHistoryRoom  = "history_room";
constexpr const char* kPresence     = "presence";
constexpr const char* kPing         = "ping";

// Server -> client events
constexpr const char* kEventMessage   = "event_message";
constexpr const char* kEventPresence  = "event_presence";
constexpr const char* kEventDelivered = "event_delivered";
constexpr const char* kAck            = "ack";
constexpr const char* kError          = "error";
constexpr const char* kPong           = "pong";
}  // namespace types

namespace errcode {
constexpr const char* kBadRequest      = "bad_request";
constexpr const char* kUnauthorized    = "unauthorized";
constexpr const char* kNotFound        = "not_found";
constexpr const char* kConflict        = "conflict";
constexpr const char* kRateLimited     = "rate_limited";
constexpr const char* kInternal        = "internal_error";
}  // namespace errcode

// Build a successful ack reply.
json make_ack(std::optional<std::int64_t> id, json data = json::object());
// Build an error reply.
json make_error(std::optional<std::int64_t> id, const std::string& code,
                const std::string& message);

// Helpers to push server-originated events. No `id`.
json make_event_message(std::int64_t msg_id, std::int64_t sender_id,
                        std::optional<std::int64_t> receiver_id,
                        std::optional<std::int64_t> room_id,
                        const std::string& content,
                        std::int64_t created_at_ms);

json make_event_presence(std::int64_t user_id, const std::string& username,
                         bool online, std::int64_t last_seen_ms);

// Try to parse a string as JSON. Returns nullopt on failure.
std::optional<json> parse(const std::string& s);

// Serialize a json with no pretty printing.
std::string serialize(const json& j);

// Convenience accessors with defaults.
std::string get_str(const json& j, const char* key, const std::string& def = "");
std::int64_t get_i64(const json& j, const char* key, std::int64_t def = 0);

}  // namespace chat::protocol
