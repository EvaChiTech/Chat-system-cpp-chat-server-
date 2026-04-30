#include "db/mysql_pool.h"
#include "common/logger.h"

#if !defined(CHAT_NO_MYSQL)
#include <mysql.h>
#include <cstring>

namespace chat::db {

MysqlPool::MysqlPool(MysqlConfig cfg) : cfg_(std::move(cfg)) {}

MysqlPool::~MysqlPool() {
  std::lock_guard<std::mutex> lk(mu_);
  stopping_ = true;
  while (!idle_.empty()) {
    auto* c = idle_.front();
    idle_.pop();
    if (c) ::mysql_close(c);
  }
  ::mysql_library_end();
}

bool MysqlPool::init() {
  if (::mysql_library_init(0, nullptr, nullptr) != 0) {
    LOG_ERROR("mysql_library_init failed");
    return false;
  }
  for (std::uint32_t i = 0; i < cfg_.pool_size; ++i) {
    auto* c = make_connection();
    if (!c) return false;
    idle_.push(c);
  }
  LOG_INFO("mysql pool ready: " << cfg_.pool_size << " connections");
  return true;
}

MYSQL* MysqlPool::make_connection() {
  MYSQL* c = ::mysql_init(nullptr);
  if (!c) return nullptr;
  bool reconnect = true;
  ::mysql_options(c, MYSQL_OPT_RECONNECT, &reconnect);
  unsigned int timeout = 5;
  ::mysql_options(c, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  if (!::mysql_real_connect(c, cfg_.host.c_str(), cfg_.user.c_str(),
                            cfg_.password.c_str(), cfg_.database.c_str(),
                            cfg_.port, nullptr, 0)) {
    LOG_ERROR("mysql_real_connect failed: " << ::mysql_error(c));
    ::mysql_close(c);
    return nullptr;
  }
  ::mysql_set_character_set(c, "utf8mb4");
  return c;
}

MysqlPool::Lease MysqlPool::acquire() {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait(lk, [&] { return !idle_.empty() || stopping_; });
  if (stopping_) return Lease{};
  auto* c = idle_.front();
  idle_.pop();
  ++outstanding_;
  return Lease(this, c);
}

void MysqlPool::release(MYSQL* c) {
  std::lock_guard<std::mutex> lk(mu_);
  --outstanding_;
  if (stopping_ || !c) {
    if (c) ::mysql_close(c);
    return;
  }
  // Best-effort liveness check: ping the server. If broken, replace the conn.
  if (::mysql_ping(c) != 0) {
    LOG_WARN("mysql_ping failed, replacing connection");
    ::mysql_close(c);
    c = make_connection();
    if (!c) return;
  }
  idle_.push(c);
  cv_.notify_one();
}

MysqlPool::Lease::~Lease() { release(); }
void MysqlPool::Lease::release() {
  if (pool_ && conn_) {
    pool_->release(conn_);
    pool_ = nullptr;
    conn_ = nullptr;
  }
}

}  // namespace chat::db

#endif  // !CHAT_NO_MYSQL
