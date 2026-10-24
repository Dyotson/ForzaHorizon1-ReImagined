// Standalone: c++ -std=c++17 tools/test_cursor_idle.cpp -o test_cursor_idle
#include "../forza-recomp/src/platform/cursor_idle.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

int main() {
  using namespace std::chrono;
  using forza::CursorIdlePolicy;
  const auto start = CursorIdlePolicy::Clock::time_point{};
  CursorIdlePolicy policy(start);
  auto update = [&](int ms, bool moved = false, bool focus = true,
                    bool overlay = false, bool captured = false) {
    return policy.Update(start + milliseconds(ms), moved, focus, overlay, captured);
  };
  CHECK(!update(2999));
  CHECK(update(3000));
  CHECK(!update(3001, true));
  CHECK(!update(6000));
  CHECK(update(6001));
  CHECK(!update(6100, false, true, true)); // Host dialog restores visibility.
  CHECK(!update(10000, false, true, true));
  CHECK(!update(10001));
  CHECK(update(13000));
  CHECK(!update(14000, false, true, false, true)); // Captured look owns hiding.
  CHECK(!update(18000, true, true, false, true));
  CHECK(!update(20000)); // Release starts a fresh idle period.
  CHECK(!update(22999));
  CHECK(update(23000));
  CHECK(!update(24000, false, false)); // Alt-tab never idles the desktop cursor.
  CHECK(!update(30000, false, false));
  CHECK(!update(30001, true));
  CHECK(update(33001));
  std::puts("cursor idle policy: PASS");
}
