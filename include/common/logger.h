#pragma once
#include <string>
#include <sstream>
#include <mutex>
#include <atomic>
#include <cstdio>

namespace chat {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

class Logger {
 public:
  static Logger& instance();

  void set_level(LogLevel l) { level_.store(static_cast<int>(l)); }
  void set_file(const std::string& path);   // empty = stderr
  void log(LogLevel l, const std::string& msg);

  static LogLevel parse_level(const std::string& s);

 private:
  Logger();
  ~Logger();
  std::atomic<int> level_{static_cast<int>(LogLevel::Info)};
  std::mutex mu_;
  FILE* sink_ = nullptr;     // owned only if file_ is true
  bool file_ = false;
};

// Convenience: stream-style log macros that lazily build the message string.
#define CHAT_LOG(level, expr)                                            \
  do {                                                                    \
    if (static_cast<int>(level) >=                                        \
        static_cast<int>(::chat::LogLevel::Trace)) {                       \
      std::ostringstream _oss;                                            \
      _oss << expr;                                                       \
      ::chat::Logger::instance().log(level, _oss.str());                   \
    }                                                                     \
  } while (0)

#define LOG_TRACE(x) CHAT_LOG(::chat::LogLevel::Trace, x)
#define LOG_DEBUG(x) CHAT_LOG(::chat::LogLevel::Debug, x)
#define LOG_INFO(x)  CHAT_LOG(::chat::LogLevel::Info,  x)
#define LOG_WARN(x)  CHAT_LOG(::chat::LogLevel::Warn,  x)
#define LOG_ERROR(x) CHAT_LOG(::chat::LogLevel::Error, x)

}  // namespace chat
