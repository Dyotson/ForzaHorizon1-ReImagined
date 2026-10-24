// forza - the scene in one pass instead of three EDRAM tiles (scene_msaa, #45, docs/perf.md
// "One-pass scene").
//
// Free roam and the races draw the 3D scene at 1280x720 with 4x MSAA, too big for the 10 MB of
// EDRAM: the game's render scenario (TilingScenario in media/renderscenarios.zip, an index
// into a table in the executable, src/platform/scene_tiling.h) splits it into three
// predicated tiles, and the emulated GPU runs the scene's commands once per tile. On a
// 4-core CPU those three passes are most of the command processor's time.
//
// scene_msaa = game (default) keeps the game's choice; 2x lowers the 4x scenario to the
// game's own 2x MSAA one (two tiles, used by its 12- and 16-car races); off lowers 4x and 2x
// to its one-pass scenario without MSAA. Two hooks:
// - Each integer render setting reads its "value" from the XML with one virtual function
//   (sub_82D7DD68, TU4 sub_82E40028); for the TilingScenario setting (its name function,
//   vtable +8, sub_82D80DD8, TU4 sub_82E42F98) the value is lowered after the read. It
//   applies from the next scenario read (startup, a race, free roam).
// - The track (static world) presentation draws each scene pass from command lists recorded
//   per (pass, layer); its scene pass is 4 with three tiles, 3 with two and 5 with one, and
//   the game records pass 4 only, so with two tiles or one the world (track, terrain,
//   barriers, signs) was missing. sub_82436468 (TU4 sub_82466418) chooses between those lists
//   and its direct path (sub_82DED198, TU4 sub_82EAF3B0: the same items drawn without
//   recorded lists) by its last argument; for passes 3 and 5 without a list it now takes the
//   direct path. The game never reaches those passes with its own scenarios in free roam
//   and races.
// tu_address_map: TU4 addresses

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include "../platform/scene_tiling.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cstdint>
#include <string>

REXCVAR_DEFINE_STRING(scene_msaa, "game", "Display",
                      "MSAA of the 3D scene: game (4x MSAA in three EDRAM tiles), 2x (two tiles), "
                      "off (one pass without MSAA: about half the GPU emulation's work)");

#if FORZA_TU4
#define ReadIntSetting sub_82E40028
#define OriginalReadIntSetting __imp__sub_82E40028
#define TrackDrawPass sub_82466418
#define OriginalTrackDrawPass __imp__sub_82466418
constexpr uint32_t kTilingScenarioName = 0x82E42F98;
#else
#define ReadIntSetting sub_82D7DD68
#define OriginalReadIntSetting __imp__sub_82D7DD68
#define TrackDrawPass sub_82436468
#define OriginalTrackDrawPass __imp__sub_82436468
constexpr uint32_t kTilingScenarioName = 0x82D80DD8;
#endif
extern "C" REX_FUNC(OriginalReadIntSetting);
extern "C" REX_FUNC(OriginalTrackDrawPass);

namespace {
namespace tiling = forza::scene_tiling;

uint32_t Load32(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Load16(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 8 | p[1];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = forza::GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

}  // namespace

// Reads an integer render setting (r3) from a render scenario XML element (r4).
extern "C" REX_FUNC(ReadIntSetting) {
  const uint32_t setting = ctx.r3.u32;
  OriginalReadIntSetting(ctx, base);
  const tiling::Mode mode = tiling::ParseMode(REXCVAR_GET(scene_msaa));
  if (mode == tiling::Mode::kGame) return;
  const uint32_t vtable = Load32(base, setting);
  if (vtable < 0x82000000 || vtable >= 0x84000000 ||
      Load32(base, vtable + 8) != kTilingScenarioName) {
    return;
  }
  const uint32_t scenario = Load32(base, setting + 4);
  const uint32_t chosen = tiling::SceneScenario(mode, scenario);
  if (chosen == scenario) return;
  Store32(base, setting + 4, chosen);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("scene_msaa: render scenario tiling {} instead of {}", chosen, scenario);
  }
}

// Draws the track presentation's items for one scene pass (r4) and layer (r6) of model
// (r8); its last argument (r10) picks the recorded lists over the direct path.
extern "C" REX_FUNC(TrackDrawPass) {
  const uint32_t pass = ctx.r4.u32;
  if ((ctx.r10.u32 & 0xFF) && (pass == tiling::kTwoTileScenePass || pass == tiling::kOneTileScenePass)) {
    // The function's own test: model->render data + 128 holds the list count (+32) and
    // the lists (+40), index pass * 3 + layer.
    const uint32_t model = Load32(base, ctx.r8.u32 + 4);
    const uint32_t data = model ? Load32(base, model + 48) : 0;
    if (data) {
      const uint32_t lists = data + 128;
      const uint32_t index = pass * 3 + ctx.r6.u32;
      bool has_list = false;
      if (index < Load16(base, lists + 32)) {
        const uint32_t list = Load32(base, Load32(base, lists + 40) + index * 4);
        has_list = list && Load32(base, list);
      }
      if (tiling::DrawTrackPassDirectly(pass, has_list)) {
        ctx.r10.u64 = 0;
      }
    }
  }
  OriginalTrackDrawPass(ctx, base);
}
