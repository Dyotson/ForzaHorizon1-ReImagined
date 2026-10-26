// scene_msaa (src/hooks/scene_tiling.cpp): which render scenario replaces the game's, whose
// recorded lists a scene pass draws, and which packets of a list recorded for the three
// tiles are skipped (src/platform/scene_tiling.h).
//   g++ -std=c++20 -I forza-recomp/src forza-recomp/tests/scene_tiling_test.cpp
#include "platform/scene_tiling.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

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
  // Lists: passes 3 and 5 (two tiles, one) draw pass 4's; the other passes their own.
  for (uint32_t pass = 0; pass < 8; ++pass) {
    const bool fewer_tiles = pass == 3 || pass == 5;
    CHECK(t::RecordedListPass(pass) == (fewer_tiles ? 4u : pass));
  }

  // A list recorded for the three tiles, as the game's start (the tile writes are repeated
  // before the first draw): per tile a bin mask and a predicated SET_CONSTANT of
  // PA_SC_WINDOW_OFFSET and the window scissor, then the render target, the viewport and the
  // MSAA config among the draw state.
  const std::vector<uint32_t> list = {
      0xC0006100, 0x00000000,                                      // 0 SET_BIN_MASK_HI 0
      0xC0006000, 0x00000003,                                      // 2 SET_BIN_MASK_LO tile 0
      0xC0032D01, 0x00040080, 0x00000000, 0x00000000, 0x01000500,  // 4 window, tile 0
      0xC0006000, 0x0000000C,                                      // 9
      0xC0032D01, 0x00040080, 0x7F000000, 0x01000000, 0x02000500,  // 11 window, tile 1
      0xC0006000, 0xFFFFFFFF,                                      // 16
      0x0000857E, 0x00010017,                                      // 18 type 0, one register
      0x80000000,                                                  // 20 type 2 padding
      0x00012200, 0x087087E7, 0x00010001,                          // 21 RB_DEPTHCONTROL...
      0x00022000, 0x14020500, 0x00030000, 0x00010400,              // 24 render target
      0x0005210F, 0x44200000, 0x44200000, 0xC3B40000, 0x43B40000,  // 28 viewport
      0xBF800000, 0x3F800000,
      0x00002301, 0x0000C003,                                      // 35 PA_SC_AA_CONFIG
      0x00062000, 1, 2, 3, 4, 5, 6, 7,                             // 37 2000-2006: COHER too
      0xC0012D00, 0x00000010, 0x3F800000,                          // 45 ALU constant 0x10
      0xC0032201, 0x00000000, 0x029C0006, 0x1240D950, 0x4000029C,  // 48 DRAW_INDX
  };
  const std::vector<size_t> bound = t::TileBoundPackets(list.data(), list.size());
  CHECK((bound == std::vector<size_t>{4, 11, 24, 35}));
  CHECK(t::NopPacket(0xC0032D01) == 0xC0031000);  // same length, predicate dropped
  CHECK(t::NopPacket(0x00022000) == 0xC0021000);
  CHECK(t::NopPacket(0x00002301) == 0xC0001000);
  // A packet cut off by the end of the buffer ends the scan.
  CHECK(t::TileBoundPackets(list.data(), 7).empty());
  CHECK((t::TileBoundPackets(list.data(), 27) == std::vector<size_t>{4, 11}));
  CHECK(t::TileBoundPackets(list.data(), 0).empty());
  std::printf("scene tiling: ok\n");
  return 0;
}
