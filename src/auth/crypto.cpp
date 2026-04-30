// SHA-256 + HMAC-SHA256 + PBKDF2 + session token.
// References: RFC 6234 (SHA-256), RFC 2104 (HMAC), RFC 8018 (PBKDF2).
#include "auth/crypto.h"
#include "common/util.h"

#include <cstring>

namespace chat::auth {

namespace {

constexpr std::uint32_t K[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

inline std::uint32_t ror(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline std::uint32_t load_be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
}
inline void store_be32(std::uint8_t* p, std::uint32_t v) {
  p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16);
  p[2] = std::uint8_t(v >> 8);  p[3] = std::uint8_t(v);
}

}  // namespace

Sha256::Sha256() {
  st_[0] = 0x6a09e667u; st_[1] = 0xbb67ae85u; st_[2] = 0x3c6ef372u; st_[3] = 0xa54ff53au;
  st_[4] = 0x510e527fu; st_[5] = 0x9b05688cu; st_[6] = 0x1f83d9abu; st_[7] = 0x5be0cd19u;
}

void Sha256::compress(const std::uint8_t* block) {
  std::uint32_t W[64];
  for (int i = 0; i < 16; ++i) W[i] = load_be32(block + i*4);
  for (int i = 16; i < 64; ++i) {
    std::uint32_t s0 = ror(W[i-15], 7) ^ ror(W[i-15], 18) ^ (W[i-15] >> 3);
    std::uint32_t s1 = ror(W[i-2], 17) ^ ror(W[i-2], 19)  ^ (W[i-2] >> 10);
    W[i] = W[i-16] + s0 + W[i-7] + s1;
  }
  std::uint32_t a=st_[0],b=st_[1],c=st_[2],d=st_[3],e=st_[4],f=st_[5],g=st_[6],h=st_[7];
  for (int i = 0; i < 64; ++i) {
    std::uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
    std::uint32_t ch = (e & f) ^ (~e & g);
    std::uint32_t t1 = h + S1 + ch + K[i] + W[i];
    std::uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
    std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
    std::uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  st_[0]+=a; st_[1]+=b; st_[2]+=c; st_[3]+=d;
  st_[4]+=e; st_[5]+=f; st_[6]+=g; st_[7]+=h;
}

void Sha256::update(const std::uint8_t* data, std::size_t n) {
  bits_ += static_cast<std::uint64_t>(n) * 8;
  while (n) {
    std::size_t take = std::min<std::size_t>(64 - blen_, n);
    std::memcpy(buf_ + blen_, data, take);
    blen_ += take; data += take; n -= take;
    if (blen_ == 64) {
      compress(buf_);
      blen_ = 0;
    }
  }
}

std::array<std::uint8_t, 32> Sha256::finish() {
  buf_[blen_++] = 0x80;
  if (blen_ > 56) {
    std::memset(buf_ + blen_, 0, 64 - blen_);
    compress(buf_);
    blen_ = 0;
  }
  std::memset(buf_ + blen_, 0, 56 - blen_);
  for (int i = 0; i < 8; ++i)
    buf_[56 + i] = static_cast<std::uint8_t>(bits_ >> (56 - 8*i));
  compress(buf_);
  std::array<std::uint8_t, 32> out;
  for (int i = 0; i < 8; ++i) store_be32(out.data() + i*4, st_[i]);
  return out;
}

std::array<std::uint8_t, 32> Sha256::hash(const std::uint8_t* d, std::size_t n) {
  Sha256 h;
  h.update(d, n);
  return h.finish();
}

std::array<std::uint8_t, 32> hmac_sha256(const std::uint8_t* key, std::size_t klen,
                                         const std::uint8_t* msg, std::size_t mlen) {
  std::uint8_t k[64] = {0};
  if (klen > 64) {
    auto h = Sha256::hash(key, klen);
    std::memcpy(k, h.data(), 32);
  } else {
    std::memcpy(k, key, klen);
  }
  std::uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  Sha256 inner;
  inner.update(ipad, 64);
  inner.update(msg, mlen);
  auto inner_h = inner.finish();
  Sha256 outer;
  outer.update(opad, 64);
  outer.update(inner_h.data(), inner_h.size());
  return outer.finish();
}

std::vector<std::uint8_t> pbkdf2_hmac_sha256(const std::string& password,
                                             const std::uint8_t* salt,
                                             std::size_t salt_len,
                                             std::uint32_t iterations,
                                             std::size_t out_len) {
  std::vector<std::uint8_t> out(out_len);
  std::size_t pos = 0;
  std::uint32_t block = 1;
  while (pos < out_len) {
    std::uint8_t  block_be[4] = {
      std::uint8_t((block >> 24) & 0xFF),
      std::uint8_t((block >> 16) & 0xFF),
      std::uint8_t((block >> 8)  & 0xFF),
      std::uint8_t( block        & 0xFF)
    };
    std::vector<std::uint8_t> salt_blk(salt, salt + salt_len);
    salt_blk.insert(salt_blk.end(), block_be, block_be + 4);
    auto u = hmac_sha256(reinterpret_cast<const std::uint8_t*>(password.data()),
                         password.size(),
                         salt_blk.data(), salt_blk.size());
    std::array<std::uint8_t, 32> t = u;
    for (std::uint32_t i = 1; i < iterations; ++i) {
      u = hmac_sha256(reinterpret_cast<const std::uint8_t*>(password.data()),
                      password.size(),
                      u.data(), u.size());
      for (int j = 0; j < 32; ++j) t[j] ^= u[j];
    }
    std::size_t take = std::min<std::size_t>(32, out_len - pos);
    std::memcpy(out.data() + pos, t.data(), take);
    pos += take;
    ++block;
  }
  return out;
}

std::string make_password_hash(const std::string& password, std::uint32_t iter) {
  std::uint8_t salt[16];
  util::random_bytes(salt, sizeof(salt));
  auto dk = pbkdf2_hmac_sha256(password, salt, sizeof(salt), iter, 32);
  std::string out;
  out += std::to_string(iter);
  out += '$';
  out += util::base64_encode(salt, sizeof(salt));
  out += '$';
  out += util::base64_encode(dk.data(), dk.size());
  return out;
}

bool verify_password_hash(const std::string& password, const std::string& stored) {
  // iter $ saltB64 $ hashB64
  auto p1 = stored.find('$');
  if (p1 == std::string::npos) return false;
  auto p2 = stored.find('$', p1 + 1);
  if (p2 == std::string::npos) return false;
  std::uint32_t iter;
  try { iter = static_cast<std::uint32_t>(std::stoul(stored.substr(0, p1))); }
  catch (...) { return false; }
  auto salt = util::base64_decode(stored.substr(p1 + 1, p2 - p1 - 1));
  auto want = util::base64_decode(stored.substr(p2 + 1));
  if (salt.empty() || want.empty()) return false;
  auto got = pbkdf2_hmac_sha256(password, salt.data(), salt.size(), iter, want.size());
  if (got.size() != want.size()) return false;
  // constant-time compare
  unsigned char diff = 0;
  for (std::size_t i = 0; i < got.size(); ++i) diff |= got[i] ^ want[i];
  return diff == 0;
}

std::string make_session_token() {
  std::uint8_t buf[32];
  util::random_bytes(buf, sizeof(buf));
  return util::hex_encode(buf, sizeof(buf));
}

}  // namespace chat::auth
