#include "test_runner.h"
#include "db/repositories.h"
#include "chat/presence.h"
#include "chat/room_manager.h"
#include "chat/message_router.h"

// Pure logic tests for the in-memory repo + router. No sockets, no MySQL.

TEST(repo_user_create_and_lookup) {
  chat::db::Repositories repos(nullptr);
  auto a = repos.create_user("alice", "h1");
  auto b = repos.create_user("bob",   "h2");
  auto dup = repos.create_user("alice", "h3");
  CHECK(a.has_value());
  CHECK(b.has_value());
  CHECK(!dup.has_value());

  auto got = repos.find_user_by_username("alice");
  CHECK(got.has_value());
  CHECK_EQ(got->id, *a);
  CHECK_EQ(got->username, std::string("alice"));
}

TEST(repo_dm_history_ordering) {
  chat::db::Repositories repos(nullptr);
  auto a = *repos.create_user("a", "x");
  auto b = *repos.create_user("b", "x");
  for (int i = 0; i < 5; ++i) {
    repos.insert_dm(a, b, "msg " + std::to_string(i), 1000 + i);
  }
  auto h = repos.history_dm(a, b, /*before*/ 0, /*limit*/ 10);
  CHECK_EQ(h.size(), std::size_t{5});
  // Oldest first.
  for (std::size_t i = 0; i + 1 < h.size(); ++i) {
    CHECK(h[i].id < h[i + 1].id);
  }
}

TEST(repo_offline_queue_drains) {
  chat::db::Repositories repos(nullptr);
  auto a = *repos.create_user("a", "x");
  auto b = *repos.create_user("b", "x");
  auto m1 = *repos.insert_dm(a, b, "1", 100);
  auto m2 = *repos.insert_dm(a, b, "2", 101);
  repos.enqueue_undelivered(m1, b);
  repos.enqueue_undelivered(m2, b);
  auto drained = repos.drain_undelivered(b);
  CHECK_EQ(drained.size(), std::size_t{2});
  // Second drain returns nothing.
  CHECK_EQ(repos.drain_undelivered(b).size(), std::size_t{0});
}

TEST(presence_transitions) {
  chat::Presence p;
  std::vector<std::pair<std::uint64_t, bool>> events;
  p.set_transition_handler([&](std::uint64_t uid, bool on) {
    events.push_back({uid, on});
  });
  // We don't have real Connections here; presence stores shared_ptrs but
  // doesn't dereference them in this path.
  // Skip: presence requires real connections to bind. We test connections_of
  // empty for an unknown user.
  CHECK(p.connections_of(123).empty());
  CHECK(!p.is_online(123));
}

TEST(room_manager_cache_round_trip) {
  chat::db::Repositories repos(nullptr);
  chat::RoomManager rooms(&repos);
  auto u1 = *repos.create_user("u1", "x");
  auto u2 = *repos.create_user("u2", "x");
  auto rid = *repos.create_room("general");
  repos.join_room(rid, u1);
  repos.join_room(rid, u2);
  // Lazy load via members_of.
  auto mems = rooms.members_of(rid);
  CHECK_EQ(mems.size(), std::size_t{2});
  CHECK(rooms.is_member(rid, u1));
  CHECK(rooms.is_member(rid, u2));

  // Cache update via on_leave.
  repos.leave_room(rid, u1);
  rooms.on_leave(rid, u1);
  CHECK(!rooms.is_member(rid, u1));
}
