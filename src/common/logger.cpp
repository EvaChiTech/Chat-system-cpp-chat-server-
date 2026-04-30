#include "common/logger.h"
#include "common/util.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace chat {

Logger& Logger::instance() {
  static Logger g;
  return g;
}

Logger::Logger() : sink_(stderr), file_(false) {}

Logger::~Logger() {
  if (file_ && sink_) std::fclose(sink_);
}

void Logger::set_file(const std::string& path) {
  std::lock_guard<std::mutex> lk(mu_);
  if (file_ && sink_) std::fclose(sink_);
  if (path.empty()) {
    sink_ = stderr;
    file_ = false;
    return;
  }
  FILE* f = std::fopen(path.c_str(), "a");
  if (!f) {
    sink_ = stderr;
    file_ = false;
    std::fprintf(stderr, "logger: failed to open %s, falling back to stderr\n",
                 path.c_str());
    return;
  }
  sink_ = f;
  file_ = true;
}

LogLevel Logger::parse_level(const std::string& s) {
  std::string l = util::to_lower(s);
  if (l == "trace") return LogLevel::Trace;
  if (l == "debug") return LogLevel::Debug;
  if (l == "info")  return LogLevel::Info;
  if (l == "warn" || l == "warning") return LogLevel::Warn;
  if (l == "error" || l == "err") return LogLevel::Error;
  return LogLevel::Info;
}

static const char* level_str(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO ";
    case LogLevel::Warn:  return "WARN ";
    case LogLevel::Error: return "ERROR";
  }
  return "?????";
}

void Logger::log(LogLevel l, const std::string& msg) {
  if (static_cast<int>(l) < level_.load()) return;

  using namespace std::chrono;
  auto now = system_clock::now();
  auto secs = system_clock::to_time_t(now);
  auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif

  // 64 bytes comfortably exceeds the worst case (~24 chars). Sized for gcc's
  // -Wformat-truncation, which can't statically prove the bounds here.
  char ts[64];
  std::snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)ms);

  std::lock_guard<std::mutex> lk(mu_);
  std::fprintf(sink_, "%s %s %s\n", ts, level_str(l), msg.c_str());
  std::fflush(sink_);
}

}  // namespace chat
