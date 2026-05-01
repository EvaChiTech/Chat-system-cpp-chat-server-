#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>

// Pull in libmysqlclient's full type definitions so this header composes
// cleanly with any TU that also includes <mysql.h> directly. Some distros
// (notably Ubuntu 22.04 / MySQL 8.0) define MYSQL as a different shape than
// the historical `typedef struct st_mysql MYSQL`, so a forward declaration
// here would conflict with mysql.h's own typedef.
#if !defined(CHAT_NO_MYSQL)
#include <mysql.h>
#endif

namespace chat::db {

struct MysqlConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 3306;
  std::string user = "root";
  std::string password;
  std::string database = "chat_system";
  std::uint32_t pool_size = 4;
};

#if !defined(CHAT_NO_MYSQL)

class MysqlPool {
 public:
  class Lease {
   public:
    Lease() = default;
    Lease(MysqlPool* p, MYSQL* c) : pool_(p), conn_(c) {}
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& o) noexcept : pool_(o.pool_), conn_(o.conn_) { o.pool_=nullptr; o.conn_=nullptr; }
    Lease& operator=(Lease&& o) noexcept {
      if (this != &o) { release(); pool_ = o.pool_; conn_ = o.conn_; o.pool_=nullptr; o.conn_=nullptr; }
      return *this;
    }
    MYSQL* get() const { return conn_; }
    explicit operator bool() const { return conn_ != nullptr; }
   private:
    void release();
    MysqlPool* pool_ = nullptr;
    MYSQL* conn_ = nullptr;
  };

  explicit MysqlPool(MysqlConfig cfg);
  ~MysqlPool();

  bool init();
  Lease acquire();
  void  release(MYSQL* c);

  const MysqlConfig& config() const { return cfg_; }

 private:
  MYSQL* make_connection();

  MysqlConfig cfg_;
  std::mutex  mu_;
  std::condition_variable cv_;
  std::queue<MYSQL*> idle_;
  std::size_t outstanding_ = 0;
  bool        stopping_ = false;
};

#else

class MysqlPool {
 public:
  class Lease { public: explicit operator bool() const { return false; } };
  explicit MysqlPool(MysqlConfig) {}
  bool init() { return true; }
  Lease acquire() { return Lease{}; }
};

#endif

}  // namespace chat::db
