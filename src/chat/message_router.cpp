#include "chat/message_router.h"
#include "db/repositories.h"
#include "protocol/messages.h"
#include "protocol/framing.h"
#include "common/util.h"
#include "common/logger.h"

namespace chat {

MessageRouter::MessageRouter(chat::db::Repositories* r, Presence* p, RoomManager* rm)
    : repos_(r), presence_(p), rooms_(rm) {}

std::uint64_t MessageRouter::send_dm(std::uint64_t sender, std::uint64_t receiver,
                                     const std::string& content) {
  auto ts = util::now_unix_ms();
  auto id = repos_->insert_dm(sender, receiver, content, ts);
  if (!id) return 0;

  auto event = protocol::serialize(
      protocol::make_event_message(*id, sender, receiver, std::nullopt, content, ts));
  auto frame = protocol::frame(event);

  auto conns = presence_->connections_of(receiver);
  if (conns.empty()) {
    repos_->enqueue_undelivered(*id, receiver);
  } else {
    for (auto& c : conns) c->send_frame(event);
    repos_->update_message_status(*id, /*delivered*/ 1);
  }

  // Echo back to other sessions of the sender (multi-device sync).
  auto own = presence_->connections_of(sender);
  for (auto& c : own) c->send_frame(event);

  return *id;
}

std::uint64_t MessageRouter::send_room(std::uint64_t sender, std::uint64_t room,
                                       const std::string& content) {
  if (!rooms_->is_member(room, sender)) return 0;

  auto ts = util::now_unix_ms();
  auto id = repos_->insert_room_msg(sender, room, content, ts);
  if (!id) return 0;

  auto event = protocol::serialize(
      protocol::make_event_message(*id, sender, std::nullopt, room, content, ts));

  auto members = rooms_->members_of(room);
  for (auto uid : members) {
    auto conns = presence_->connections_of(uid);
    if (conns.empty()) {
      // Room messages also queue for offline members so they can catch up.
      if (uid != sender) repos_->enqueue_undelivered(*id, uid);
      continue;
    }
    for (auto& c : conns) c->send_frame(event);
  }
  return *id;
}

void MessageRouter::replay_offline(std::uint64_t user_id) {
  auto pending = repos_->drain_undelivered(user_id);
  if (pending.empty()) return;
  auto conns = presence_->connections_of(user_id);
  if (conns.empty()) return;
  for (auto& m : pending) {
    auto j = protocol::make_event_message(
        m.id, m.sender_id, m.receiver_id, m.room_id, m.content, m.created_at_ms);
    auto s = protocol::serialize(j);
    for (auto& c : conns) c->send_frame(s);
  }
  LOG_DEBUG("replay_offline: " << pending.size() << " messages for uid=" << user_id);
}

}  // namespace chat
