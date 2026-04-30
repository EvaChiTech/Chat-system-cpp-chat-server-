#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>

namespace chat {

// Plain "KEY = VALUE" file loader, with environment-variable overlay.
// Env var names are uppercased keys (e.g. mysql_host -> MYSQL_HOST).
class Config {
 public:
  bool load_file(const std::string& path);
  void overlay_env();

  std::string get(const std::string& key, const std::string& def = "") const;
  int    get_int (const std::string& key, int    def) const;
  std::uint32_t get_u32(const std::string& key, std::uint32_t def) const;
  bool   get_bool(const std::string& key, bool   def) const;

  void set(const std::string& key, const std::string& value);
  const std::unordered_map<std::string,std::string>& all() const { return kv_; }

 private:
  std::unordered_map<std::string,std::string> kv_;
};

}  // namespace chat
