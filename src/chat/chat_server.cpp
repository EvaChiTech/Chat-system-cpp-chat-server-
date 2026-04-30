#include "chat/chat_server.h"
#include "handlers/handlers.h"
#include "common/logger.h"
#include "common/util.h"
#include "protocol/messages.h"

#include <thread>

namespace chat {

ChatServer::ChatServer(Config c) : cfg_(std::move(c)) {}
ChatServer::~ChatServer() { stop(); }

bool ChatServer::init() {
  if (!chat::platform::init()) {
    LOG_ERROR("platform init failed");
    return false;
  }

  auto level = Logger::parse_level(cfg_.get("log_level", "info"));
  Logger::instance().set_level(level);
  auto lf = cfg_.get("log_file", "");
  if (!lf.empty()) Logger::instance().set_file(lf);

  max_frame_   = cfg_.get_u32("max_frame_bytes",   65536);
  max_message_ = cfg_.get_u32("max_message_bytes", 4096);
  pbkdf2_iter_ = cfg_.get_u32("pbkdf2_iterations", 120000);

  // DB pool
  chat::db::MysqlConfig mc;
  mc.host      = cfg_.get("mysql_host", "127.0.0.1");
  mc.port      = static_cast<std::uint16_t>(cfg_.get_int("mysql_port", 3306));
  mc.user      = cfg_.get("mysql_user", "root");
  mc.password  = cfg_.get("mysql_password", "");
  mc.database  = cfg_.get("mysql_database", "chat_system");
  mc.pool_size = cfg_.get_u32("mysql_pool_size", 4);

  db_pool_ = std::make_unique<chat::db::MysqlPool>(mc);
  if (!db_pool_->init()) {
    LOG_ERROR("mysql pool init failed");
    return false;
  }
  repos_    = std::make_unique<chat::db::Repositories>(db_pool_.get());
  sessions_ = std::make_unique<chat::auth::SessionManager>(
      repos_.get(), cfg_.get_int("session_ttl_seconds", 604800));
  sessions_->warm();

  presence_ = std::make_unique<Presence>();
  rooms_    = std::make_unique<RoomManager>(repos_.get());
  router_   = std::make_unique<MessageRouter>(repos_.get(), presence_.get(),
                                              rooms_.get());

  presence_->set_transition_handler(
      [this](std::uint64_t uid, bool online) {
        if (!online) {
          repos_->update_last_seen(uid, util::now_unix_ms());
        } else {
          // Replay any messages queued while offline.
          router_->replay_offline(uid);
        }
        // (Stage 2) broadcast presence transitions to followers.
        (void)uid;
      });

  // Reactor + thread pool + server
  reactor_ = net::Reactor::create();
  if (!reactor_ || !reactor_->init()) {
    LOG_ERROR("reactor init failed");
    return false;
  }

  std::uint32_t nworkers = cfg_.get_u32("worker_threads", 0);
  if (nworkers == 0) nworkers = std::max(2u, std::thread::hardware_concurrency());
  pool_ = std::make_unique<ThreadPool>(nworkers);
  LOG_INFO("worker threads: " << nworkers);

  server_ = std::make_unique<net::Server>(reactor_.get(), max_frame_);
  server_->set_handlers(
      [this](auto c) { on_connect(std::move(c)); },
      [this](auto c, auto p) { on_frame(std::move(c), std::move(p)); },
      [this](auto c) { on_close(std::move(c)); });

  std::string host = cfg_.get("listen_host", "0.0.0.0");
  std::uint16_t port = static_cast<std::uint16_t>(cfg_.get_int("listen_port", 9000));
  if (!server_->listen(host, port)) {
    LOG_ERROR("server failed to listen");
    return false;
  }
  return true;
}

void ChatServer::run() {
  if (!reactor_) return;
  reactor_->run();
}

void ChatServer::stop() {
  if (server_) server_->stop();
  if (reactor_) reactor_->stop();
  if (pool_) pool_->shutdown();
}

void ChatServer::on_connect(std::shared_ptr<net::Connection> c) {
  LOG_DEBUG("conn[" << c->id() << "] connect");
}

void ChatServer::on_close(std::shared_ptr<net::Connection> c) {
  auto uid = c->user_id();
  if (uid) presence_->unbind(uid, c);
  LOG_DEBUG("conn[" << c->id() << "] close uid=" << uid);
}

void ChatServer::on_frame(std::shared_ptr<net::Connection> c, std::string payload) {
  // Hand off to the worker pool so the I/O thread doesn't stall on DB / hashing.
  auto self = this;
  pool_->submit([self, c = std::move(c), payload = std::move(payload)] {
    handlers::dispatch(self, c, payload);
  });
}

}  // namespace chat
