// forza - the scene's MSAA and EDRAM tiling (scene_msaa, src/hooks/scene_tiling.cpp).
// The decisions, without guest memory, so tests/scene_tiling_test.cpp can check them.
#pragma once

#include <cstdint>
#include <string_view>

namespace forza::scene_tiling {

// The game's TilingScenario values (media/renderscenarios.zip), entries of a table in the
// executable: tile rectangles and MSAA of the 1280x720 scene.
constexpr uint32_t k4xMsaa = 0;        // three 1280x256 tiles (the last 208 rows), 4x MSAA
constexpr uint32_t k2xMsaa = 1;        // two tiles, 2x MSAA
constexpr uint32_t kNonTiling = 2;     // one pass, no MSAA
constexpr uint32_t kSmall2xMsaa = 3;   // one pass, 2x MSAA, 1120x584
constexpr uint32_t kSplitScreen = 4;   // one 1280x368 pass per player, 2x MSAA

enum class Mode { kGame, k2x, kOff };

inline Mode ParseMode(std::string_view value) {
  if (value == "2x") return Mode::k2x;
  if (value == "off") return Mode::kOff;
  return Mode::kGame;
}

// The scenario to render with instead of the game's: only the full-screen 4x and 2x
// scenarios are lowered; split screen and the others stay.
inline uint32_t SceneScenario(Mode mode, uint32_t scenario) {
  if (mode == Mode::k2x && scenario == k4xMsaa) return k2xMsaa;
  if (mode == Mode::kOff && (scenario == k4xMsaa || scenario == k2xMsaa)) return kNonTiling;
  return scenario;
}

// The track (static world) presentation draws each scene pass from lists recorded per
// (pass, layer), index pass * 3 + layer. Its scene pass is 4 with three tiles, 3 with two
// and 5 with one; the game records lists for pass 4 only, so in a two-tile or one-tile
// scene the track drew nothing. Such a pass is drawn with the presentation's direct path
// (its other branch, which draws the same items without recorded lists) instead.
constexpr uint32_t kTwoTileScenePass = 3;
constexpr uint32_t kOneTileScenePass = 5;
inline bool DrawTrackPassDirectly(uint32_t pass, bool has_recorded_list) {
  return !has_recorded_list && (pass == kTwoTileScenePass || pass == kOneTileScenePass);
}

}  // namespace forza::scene_tiling
