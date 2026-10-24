// scene_msaa (src/hooks/scene_tiling.cpp): which render scenario replaces the game's, and
// which track passes are drawn without recorded lists (src/platform/scene_tiling.h).
//   g++ -std=c++20 -I forza-recomp/src forza-recomp/tests/scene_tiling_test.cpp
#include "platform/scene_tiling.h"

#include <cstdio>
#include <cstdlib>

namespace t = forza::scene_tiling;

#define CHECK(x)                                                \
  do {                                                          \
    if (!(x)) {                                                 \
      std::fprintf(stderr, "scene tiling: FAIL: %s\n", #x);     \
      std::exit(1);                                             \
    }                                                           \
  } while (false)

int main() {
  CHECK(t::ParseMode("game") == t::Mode::kGame);
  CHECK(t::ParseMode("") == t::Mode::kGame);
  CHECK(t::ParseMode("4x") == t::Mode::kGame);
  CHECK(t::ParseMode("2x") == t::Mode::k2x);
  CHECK(t::ParseMode("off") == t::Mode::kOff);
  for (uint32_t s = 0; s < 8; ++s) {
    // The game's choice is kept as it is.
    CHECK(t::SceneScenario(t::Mode::kGame, s) == s);
  }
  // 2x: only the 4x scenario becomes the game's 2x one.
  CHECK(t::SceneScenario(t::Mode::k2x, t::k4xMsaa) == t::k2xMsaa);
  CHECK(t::SceneScenario(t::Mode::k2x, t::k2xMsaa) == t::k2xMsaa);
  CHECK(t::SceneScenario(t::Mode::k2x, t::kNonTiling) == t::kNonTiling);
  // off: 4x and 2x become the one-pass scenario.
  CHECK(t::SceneScenario(t::Mode::kOff, t::k4xMsaa) == t::kNonTiling);
  CHECK(t::SceneScenario(t::Mode::kOff, t::k2xMsaa) == t::kNonTiling);
  // Split screen and the small one-pass scenario stay.
  for (auto m : {t::Mode::k2x, t::Mode::kOff}) {
    CHECK(t::SceneScenario(m, t::kSplitScreen) == t::kSplitScreen);
    CHECK(t::SceneScenario(m, t::kSmall2xMsaa) == t::kSmall2xMsaa);
  }
  // The direct path: only the two-tile and one-tile scene passes, only without a list.
  for (uint32_t pass = 0; pass < 8; ++pass) {
    const bool scene_pass = pass == 3 || pass == 5;
    CHECK(t::DrawTrackPassDirectly(pass, false) == scene_pass);
    CHECK(!t::DrawTrackPassDirectly(pass, true));
  }
  std::printf("scene tiling: ok\n");
  return 0;
}
