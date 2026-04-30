#include "common/util.h"
#include "common/platform.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>

#if !defined(_WIN32)
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace chat::platform {
#if defined(_WIN32)
static bool g_wsa_init = false;
bool init() {
  if (g_wsa_init) return true;
  WSADATA w;
  if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
  g_wsa_init = true;
  return true;
}
void shutdown() {
  if (g_wsa_init) { WSACleanup(); g_wsa_init = false; }
}
#else
bool init() { return true; }
void shutdown() {}
#endif
}  // namespace chat::platform

namespace chat::util {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char d) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == d) { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string to_upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return s;
}

static const char kHex[] = "0123456789abcdef";

std::string hex_encode(const std::uint8_t* data, std::size_t n) {
  std::string out;
  out.resize(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out[i*2]   = kHex[(data[i] >> 4) & 0xF];
    out[i*2+1] = kHex[ data[i]       & 0xF];
  }
  return out;
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

std::vector<std::uint8_t> hex_decode(const std::string& s) {
  std::vector<std::uint8_t> out;
  if (s.size() % 2 != 0) return out;
  out.reserve(s.size() / 2);
  for (std::size_t i = 0; i < s.size(); i += 2) {
    int hi = hex_val(s[i]);
    int lo = hex_val(s[i+1]);
    if (hi < 0 || lo < 0) return {};
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

static const char kB64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::uint8_t* data, std::size_t n) {
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= n) {
    std::uint32_t v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
    out.push_back(kB64[(v >> 18) & 0x3F]);
    out.push_back(kB64[(v >> 12) & 0x3F]);
    out.push_back(kB64[(v >> 6)  & 0x3F]);
    out.push_back(kB64[ v        & 0x3F]);
    i += 3;
  }
  if (i < n) {
    std::uint32_t v = data[i] << 16;
    if (i + 1 < n) v |= data[i+1] << 8;
    out.push_back(kB64[(v >> 18) & 0x3F]);
    out.push_back(kB64[(v >> 12) & 0x3F]);
    out.push_back((i + 1 < n) ? kB64[(v >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
  return out;
}

std::vector<std::uint8_t> base64_decode(const std::string& s) {
  static int8_t T[256];
  static bool inited = false;
  if (!inited) {
    for (int i = 0; i < 256; ++i) T[i] = -1;
    for (int i = 0; i < 64; ++i) T[(unsigned char)kB64[i]] = i;
    inited = true;
  }
  std::vector<std::uint8_t> out;
  std::uint32_t v = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int8_t t = T[(unsigned char)c];
    if (t < 0) return {};
    v = (v << 6) | static_cast<std::uint32_t>(t);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((v >> bits) & 0xFF));
    }
  }
  return out;
}

void random_bytes(std::uint8_t* out, std::size_t n) {
#if !defined(_WIN32)
  // Prefer /dev/urandom on POSIX — std::random_device on glibc reads it,
  // but we open it directly to avoid surprises and to be explicit.
  int fd = ::open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    std::size_t got = 0;
    while (got < n) {
      auto r = ::read(fd, out + got, n - got);
      if (r <= 0) break;
      got += static_cast<std::size_t>(r);
    }
    ::close(fd);
    if (got == n) return;
    // fall through to RNG below
  }
#endif
  std::random_device rd;
  std::mt19937_64 g(((std::uint64_t)rd() << 32) ^ rd());
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::uint8_t>(g() & 0xFF);
  }
}

std::int64_t now_steady_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t now_unix_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

}  // namespace chat::util
