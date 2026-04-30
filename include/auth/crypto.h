#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace chat::auth {

// SHA-256 -- one-shot and streaming.
struct Sha256 {
  Sha256();
  void update(const std::uint8_t* data, std::size_t n);
  std::array<std::uint8_t, 32> finish();

  static std::array<std::uint8_t, 32> hash(const std::uint8_t* data, std::size_t n);

 private:
  void compress(const std::uint8_t* block);
  std::uint32_t st_[8];
  std::uint8_t  buf_[64];
  std::uint64_t bits_ = 0;
  std::size_t   blen_ = 0;
};

std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t klen,
                                         const std::uint8_t* msg, std::size_t mlen);

// Returns `out_len` bytes derived from password+salt using PBKDF2-HMAC-SHA256.
std::vector<std::uint8_t> pbkdf2_hmac_sha256(const std::string& password,
                                             const std::uint8_t* salt,
                                             std::size_t salt_len,
                                             std::uint32_t iterations,
                                             std::size_t out_len);

// Format: "iter$saltB64$hashB64". Uses 16-byte salt, 32-byte derived key.
std::string make_password_hash(const std::string& password,
                               std::uint32_t iterations);

// Constant-time compare; returns true on match.
bool verify_password_hash(const std::string& password,
                          const std::string& stored);

// 32-byte session token, hex-encoded -> 64 chars.
std::string make_session_token();

}  // namespace chat::auth
