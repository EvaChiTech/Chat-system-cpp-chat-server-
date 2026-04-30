#include "test_runner.h"
#include "protocol/framing.h"
#include "auth/crypto.h"
#include "common/util.h"

#include <stdexcept>

namespace ct = chat::test;

TEST(frame_encode_decode_roundtrip) {
  std::string payload = "{\"type\":\"ping\",\"id\":42}";
  auto framed = chat::protocol::frame(payload);
  CHECK_EQ(framed.size(), payload.size() + 4);
  // Length is big-endian.
  CHECK_EQ(static_cast<unsigned char>(framed[0]), 0u);
  CHECK_EQ(static_cast<unsigned char>(framed[1]), 0u);
  CHECK_EQ(static_cast<unsigned char>(framed[2]), 0u);
  CHECK_EQ(static_cast<unsigned char>(framed[3]),
           static_cast<unsigned char>(payload.size()));

  chat::protocol::FrameDecoder d;
  auto frames = d.feed(framed.data(), framed.size());
  CHECK_EQ(frames.size(), std::size_t{1});
  CHECK_EQ(frames[0], payload);
}

TEST(frame_decoder_handles_partial_and_multi) {
  chat::protocol::FrameDecoder d;
  auto a = chat::protocol::frame("hello");
  auto b = chat::protocol::frame("world!");
  std::string both = a + b;
  // Feed in 1-byte chunks.
  std::vector<std::string> all;
  for (char c : both) {
    auto v = d.feed(&c, 1);
    for (auto& s : v) all.push_back(s);
  }
  CHECK_EQ(all.size(), std::size_t{2});
  CHECK_EQ(all[0], "hello");
  CHECK_EQ(all[1], "world!");
}

TEST(frame_decoder_rejects_oversize) {
  chat::protocol::FrameDecoder d;
  d.max_frame_bytes = 4;
  std::string huge = chat::protocol::frame("toolong");
  bool threw = false;
  try { d.feed(huge.data(), huge.size()); }
  catch (const std::exception&) { threw = true; }
  CHECK(threw);
}

// SHA-256 known-answer tests
TEST(sha256_known_answers) {
  // Empty
  auto h = chat::auth::Sha256::hash(nullptr, 0);
  std::string hex = chat::util::hex_encode(h.data(), h.size());
  CHECK_EQ(hex,
    std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  // "abc"
  std::string s = "abc";
  auto h2 = chat::auth::Sha256::hash(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  CHECK_EQ(chat::util::hex_encode(h2.data(), h2.size()),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(password_hash_roundtrip) {
  std::string h = chat::auth::make_password_hash("hunter2", 1000);
  CHECK(chat::auth::verify_password_hash("hunter2", h));
  CHECK(!chat::auth::verify_password_hash("Hunter2", h));
  CHECK(!chat::auth::verify_password_hash("", h));
}

TEST(base64_roundtrip) {
  std::vector<std::uint8_t> v = {0,1,2,3,4,5,6,7,8,9,10,255};
  auto enc = chat::util::base64_encode(v.data(), v.size());
  auto dec = chat::util::base64_decode(enc);
  CHECK_EQ(dec.size(), v.size());
  for (std::size_t i = 0; i < v.size(); ++i) CHECK_EQ(dec[i], v[i]);
}
