#include "test_runner.h"
#include <cstdio>

int main() {
  int pass = 0, fail = 0;
  for (auto& c : chat::test::registry()) {
    std::printf("[ RUN  ] %s\n", c.name);
    try { c.fn(); std::printf("[  OK  ] %s\n", c.name); ++pass; }
    catch (const std::exception& e) {
      std::printf("[ FAIL ] %s: %s\n", c.name, e.what()); ++fail;
    } catch (...) {
      std::printf("[ FAIL ] %s: unknown\n", c.name); ++fail;
    }
  }
  std::printf("---\n%d passed, %d failed\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
