#include "common/config.h"
#include "common/util.h"
#include "common/logger.h"

#include <fstream>
#include <cstdlib>

namespace chat {

bool Config::load_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    LOG_WARN("config: cannot open " << path);
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    auto t = util::trim(line);
    if (t.empty() || t[0] == '#') continue;
    auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    auto key = util::to_lower(util::trim(t.substr(0, eq)));
    auto val = util::trim(t.substr(eq + 1));
    kv_[key] = val;
  }
  return true;
}

void Config::overlay_env() {
  // Build a snapshot of keys to look up so we don't iterate while inserting.
  std::vector<std::string> keys;
  keys.reserve(kv_.size());
  for (auto& [k, _] : kv_) keys.push_back(k);
  // Also a fixed list of well-known keys that env may set without a file.
  static const char* well_known[] = {
    "listen_host", "listen_port", "worker_threads",
    "log_level", "log_file",
    "mysql_host", "mysql_port", "mysql_user", "mysql_password",
    "mysql_database", "mysql_pool_size",
    "session_ttl_seconds", "pbkdf2_iterations",
    "max_message_bytes", "max_frame_bytes",
  };
  for (auto k : well_known) keys.emplace_back(k);

  for (auto& key : keys) {
    auto upper = util::to_upper(key);
    if (const char* env = std::getenv(upper.c_str())) {
      kv_[key] = env;
    }
  }
}

std::string Config::get(const std::string& key, const std::string& def) const {
  auto it = kv_.find(util::to_lower(key));
  return it == kv_.end() ? def : it->second;
}

int Config::get_int(const std::string& key, int def) const {
  auto v = get(key, "");
  if (v.empty()) return def;
  try { return std::stoi(v); } catch (...) { return def; }
}

std::uint32_t Config::get_u32(const std::string& key, std::uint32_t def) const {
  auto v = get(key, "");
  if (v.empty()) return def;
  try { return static_cast<std::uint32_t>(std::stoul(v)); } catch (...) { return def; }
}

bool Config::get_bool(const std::string& key, bool def) const {
  auto v = util::to_lower(get(key, ""));
  if (v.empty()) return def;
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return def;
}

void Config::set(const std::string& k, const std::string& v) {
  kv_[util::to_lower(k)] = v;
}

}  // namespace chat
