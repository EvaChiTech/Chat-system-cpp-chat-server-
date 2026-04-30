#pragma once
#include "chat/presence.h"
#include "chat/room_manager.h"

#include <cstdint>
#include <string>

namespace chat::db { class Repositories; }

namespace chat {

// Stitches together persistence + presence + rooms. This is the place where
// "new message" turns into "stored + delivered to N connections".
class MessageRouter {
 public:
  MessageRouter(chat::db::Repositories* repos,
                Presence* presence,
                RoomManager* rooms);

  // Persist a DM and deliver it to whichever sessions of `receiver` are
  // online. If receiver is offline, the message is enqueued in the offline
  // table for replay on next login.
  // `sender_id` and `receiver_id` are pre-validated.
  // Returns the new message_id, or 0 on failure.
  std::uint64_t send_dm(std::uint64_t sender_id, std::uint64_t receiver_id,
                        const std::string& content);

  // Persist a room message and fan it out to every online member except sender.
  std::uint64_t send_room(std::uint64_t sender_id, std::uint64_t room_id,
                          const std::string& content);

  // Replay queued messages to a freshly-connected user.
  void replay_offline(std::uint64_t user_id);

 private:
  chat::db::Repositories* repos_;
  Presence*               presence_;
  RoomManager*            rooms_;
};

}  // namespace chat
