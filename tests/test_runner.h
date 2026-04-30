// Minimal test runner shared across test translation units.
#pragma once
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace chat::test {
struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& registry() { static std::vector<Case> v; return v; }
struct Reg { Reg(const char* n, void (*f)()) { registry().push_back({n,f}); } };
}  // namespace chat::test

#define TEST(name) \
  static void test_##name(); \
  static ::chat::test::Reg reg_##name(#name, &test_##name); \
  static void test_##name()

#define CHECK(cond) do { \
  if (!(cond)) { \
    std::fprintf(stderr, "  CHECK failed: %s @ %s:%d\n", #cond, __FILE__, __LINE__); \
    throw std::runtime_error("check failed"); \
  } \
} while (0)

#define CHECK_EQ(a, b) do { \
  if (!((a) == (b))) { \
    std::fprintf(stderr, "  CHECK_EQ failed: %s == %s @ %s:%d\n", #a, #b, __FILE__, __LINE__); \
    throw std::runtime_error("check failed"); \
  } \
} while (0)
