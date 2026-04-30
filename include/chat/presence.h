#pragma once
#include "net/connection.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chat {

// Tracks which user_ids are currently online and on which connection(s).
// A user can be connected from multiple devices simultaneously.
class Presence {
 public:
  // Called whenever a user transitions online -> offline or vice versa.
  using TransitionHandler = std::function<void(std::uint64_t user_id, bool online)>;

  void set_transition_handler(TransitionHandler h) { handler_ = std::move(h); }

  // Bind/unbind. on_bind returns true on online transition.
  bool bind(std::uint64_t user_id, std::shared_ptr<net::Connection> conn);
  bool unbind(std::uint64_t user_id, std::shared_ptr<net::Connection> conn);

  // All connections for `user_id` (empty if offline).
  std::vector<std::shared_ptr<net::Connection>>
  connections_of(std::uint64_t user_id);

  bool is_online(std::uint64_t user_id);

 private:
  std::mutex mu_;
  // user_id -> set of conn_ids -> Connection
  std::unordered_map<std::uint64_t,
                     std::unordered_map<std::uint64_t,
                                        std::shared_ptr<net::Connection>>> map_;
  TransitionHandler handler_;
};

}  // namespace chat
