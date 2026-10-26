// forza - the scene's MSAA and EDRAM tiling (scene_msaa, src/hooks/scene_tiling.cpp).
// The decisions, without guest memory, so tests/scene_tiling_test.cpp can check them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

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

// The track (static world) presentation and its procedural models (trees, barriers, ...)
// draw each scene pass from command lists recorded per (pass, layer), index pass * 3 + layer.
// The scene pass is 4 with three tiles, 3 with two and 5 with one; the game's model data
// hold lists for pass 4 only, so with two tiles or one those items drew nothing. Passes 3
// and 5 draw pass 4's lists instead, with the packets that tie them to the three tiles
// skipped (TileBoundPackets).
constexpr uint32_t kTwoTileScenePass = 3;
constexpr uint32_t kThreeTileScenePass = 4;
constexpr uint32_t kOneTileScenePass = 5;
inline uint32_t RecordedListPass(uint32_t pass) {
  return pass == kTwoTileScenePass || pass == kOneTileScenePass ? kThreeTileScenePass : pass;
}

// A list recorded for the three tiles holds their layout in these PM4 packets, which the
// scene's own state already covers when the list runs in a scene of two tiles or one:
// - writes of PA_SC_WINDOW_OFFSET and the window scissor (registers 0x2080-0x2082), one per
//   tile under that tile's bin mask, moving the window to the tile (and the screen scissor,
//   0x200E-0x200F);
// - writes of the render target (RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO,
//   RB_COLOR1-3_INFO: 0x2000-0x2005, pitch, 4x MSAA and the EDRAM bases of a 1280x256 tile)
//   and of PA_SC_AA_CONFIG / PA_SC_AA_MASK (0x2301, 0x2312: four samples).
// Left in, they move the window to a tile's band and switch the render target to the tiled
// EDRAM layout (the sky was drawn into the bottom of the screen, the rest of the scene was
// lost). TileBoundPackets returns the header index of each such packet in one indirect
// buffer (words in host byte order); NopPacket is a type-3 NOP of the same length to put in
// its place.
constexpr bool LayoutRegister(uint32_t reg) {
  return (reg >= 0x2000 && reg <= 0x2005) || reg == 0x200E || reg == 0x200F ||
         (reg >= 0x2080 && reg <= 0x2082) || reg == 0x2301 || reg == 0x2312;
}
constexpr uint32_t kPm4SetConstant = 0x2D;
constexpr uint32_t kPm4Nop = 0x10;
constexpr uint32_t NopPacket(uint32_t header) {
  return 0xC0000000u | (header & 0x3FFF0000u) | (kPm4Nop << 8);
}
inline std::vector<size_t> TileBoundPackets(const uint32_t* words, size_t count) {
  std::vector<size_t> found;
  size_t i = 0;
  while (i < count) {
    const uint32_t header = words[i];
    const uint32_t data = ((header >> 16) & 0x3FFF) + 1;
    switch (header >> 30) {
      case 0: {  // registers: base, consecutive or (bit 15) all to the base one
        if (i + data >= count) return found;
        const uint32_t first = header & 0x7FFF;
        const uint32_t last = header & 0x8000 ? first : first + data - 1;
        bool all = true;
        for (uint32_t reg = first; reg <= last && all; ++reg) all = LayoutRegister(reg);
        if (all) found.push_back(i);
        i += 1 + data;
        break;
      }
      case 1:  // two registers
        i += 3;
        break;
      case 2:  // padding
        i += 1;
        break;
      default: {
        if (i + data >= count) return found;
        // SET_CONSTANT of registers: type 4 in the first data word, then the values.
        if (((header >> 8) & 0x7F) == kPm4SetConstant && data >= 2 &&
            ((words[i + 1] >> 16) & 0xFF) == 4) {
          const uint32_t first = 0x2000 + (words[i + 1] & 0x7FF);
          bool all = true;
          for (uint32_t reg = first; reg < first + data - 1 && all; ++reg) {
            all = LayoutRegister(reg);
          }
          if (all) found.push_back(i);
        }
        i += 1 + data;
        break;
      }
    }
  }
  return found;
}

}  // namespace forza::scene_tiling
