#pragma once
#include "common/config.h"
#include "common/thread_pool.h"
#include "net/reactor.h"
#include "net/server.h"
#include "auth/session.h"
#include "db/mysql_pool.h"
#include "db/repositories.h"
#include "chat/presence.h"
#include "chat/room_manager.h"
#include "chat/message_router.h"

#include <memory>

namespace chat {

// Top-level orchestrator. Owns the reactor, thread pool, server, DB, auth,
// and chat domain components. main() builds one of these and calls run().
class ChatServer {
 public:
  explicit ChatServer(Config cfg);
  ~ChatServer();

  bool init();
  void run();        // blocks
  void stop();

  // Accessors for handlers.
  chat::db::Repositories*      repos()    { return repos_.get(); }
  chat::auth::SessionManager*  sessions() { return sessions_.get(); }
  Presence*                    presence() { return presence_.get(); }
  RoomManager*                 rooms()    { return rooms_.get(); }
  MessageRouter*               router()   { return router_.get(); }

  std::uint32_t max_message() const { return max_message_; }
  std::uint32_t pbkdf2_iter() const { return pbkdf2_iter_; }

 private:
  void on_connect(std::shared_ptr<net::Connection> c);
  void on_close  (std::shared_ptr<net::Connection> c);
  void on_frame  (std::shared_ptr<net::Connection> c, std::string payload);

  Config                              cfg_;
  std::unique_ptr<net::Reactor>       reactor_;
  std::unique_ptr<ThreadPool>         pool_;
  std::unique_ptr<net::Server>        server_;

  std::unique_ptr<chat::db::MysqlPool>     db_pool_;
  std::unique_ptr<chat::db::Repositories>  repos_;
  std::unique_ptr<chat::auth::SessionManager> sessions_;

  std::unique_ptr<Presence>      presence_;
  std::unique_ptr<RoomManager>   rooms_;
  std::unique_ptr<MessageRouter> router_;

  std::uint32_t max_frame_ = 65536;
  std::uint32_t max_message_ = 4096;
  std::uint32_t pbkdf2_iter_ = 120000;
};

}  // namespace chat
