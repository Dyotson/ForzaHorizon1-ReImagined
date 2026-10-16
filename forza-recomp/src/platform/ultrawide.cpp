// View-state strategy adapted from Pinyon Shift contributors (BSD-3-Clause).
// See patches/rexglue-sdk/LICENSE-Pinyon-Shift.
#include "ultrawide.h"
#include "ultrawide_math.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <mutex>
#ifdef _WIN32
std::string ForzaGpuBackend();
#endif
REXCVAR_DEFINE_BOOL(forza_ultrawide, false, "Display", "Widen gameplay to the window; keep menus at 16:9");
REXCVAR_DEFINE_BOOL(forza_hud_edges, false, "Display", "Anchor side HUD elements to the ultrawide edges");
REXCVAR_DEFINE_BOOL(forza_hud_layout, false, "Display", "Customize HUD safe area and position during gameplay");
REXCVAR_DEFINE_DOUBLE(forza_hud_safe_width, 100.0, "Display", "HUD safe rectangle width percent")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(forza_hud_safe_height, 100.0, "Display", "HUD safe rectangle height percent")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(forza_hud_offset_x, 0.0, "Display", "HUD horizontal offset, percent of screen")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(forza_hud_offset_y, 0.0, "Display", "HUD vertical offset, percent of screen")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
namespace forza::ultrawide {
namespace {
std::atomic<uint64_t> size{}, frame{}, gameplay_frame{}, map_frame{};
std::atomic<double> aspect_scale{1.0};
std::atomic<bool> wide{}, ready{};
Presentation present;
std::mutex frame_mutex;
}
void Resize(uint32_t width, uint32_t height) { size.store((uint64_t(width)<<32)|height); }
void Prepare(uint32_t width, uint32_t height) {
  if (!REXCVAR_GET(forza_ultrawide)) return;
  bool supported = false;
  for (const auto& flag : rex::cvar::GetRegistry()) supported |= flag.name == "forza_native_width";
#ifdef _WIN32
  supported &= ForzaGpuBackend() == "vulkan";
#endif
  if (!supported || !width || !height || width > 1280*7 || height > 720*7) {
    rex::cvar::SetFlagByName("forza_ultrawide", "false");
    REXLOG_WARN("Native ultrawide requires the patched Vulkan renderer and a supported window size");
    return;
  }
  // Integer scales describe tiled storage only. Rasterization and sampling use
  // the physical window dimensions; the padded columns are not extra scenery.
  const auto x = RenderScale(width, 1280, 1), y = RenderScale(height, 720, 1);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(x));
  rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(y));
  rex::cvar::SetFlagByName("forza_native_width", std::to_string(width));
  rex::cvar::SetFlagByName("forza_native_height", std::to_string(height));
  REXLOG_INFO("Ultrawide native drawing/output {}x{}; tiled storage stride {}x{}", width, height, 1280*x, 720*y);
}
void Initialize(uint32_t width, uint32_t height, Presentation callback) {
  Resize(width,height);
  present = std::move(callback);
  bool supported = false;
  for (const auto& flag : rex::cvar::GetRegistry()) supported |= flag.name == "forza_ultrawide_hud_x";
  bool edges_supported = false;
  for (const auto& flag : rex::cvar::GetRegistry())
    edges_supported |= flag.name == "forza_ultrawide_hud_edge_shift";
  if (REXCVAR_GET(forza_hud_edges) && !edges_supported) {
    rex::cvar::SetFlagByName("forza_hud_edges", "false");
    REXLOG_WARN("Expanded HUD requires the HUD edge-anchor SDK patch; retaining centered HUD");
  }
  ready = (REXCVAR_GET(forza_ultrawide) || REXCVAR_GET(forza_hud_layout)) && supported;
  if (ready.load()) {
    rex::cvar::SetFlagByName("forza_present_16_9", "true");
    rex::cvar::SetFlagByName("present_letterbox", "true");
  }
  if ((REXCVAR_GET(forza_ultrawide) || REXCVAR_GET(forza_hud_layout)) && !supported)
    REXLOG_WARN("Ultrawide disabled: rebuild with the ultrawide SDK patch");
}
void Frame() {
  if (!ready.load()) return;
  std::lock_guard lock(frame_mutex);
  const auto n = ++frame, game = gameplay_frame.load(), map = map_frame.load(), dimensions = size.load();
  const bool custom = REXCVAR_GET(forza_hud_layout);
  const bool ultrawide = REXCVAR_GET(forza_ultrawide);
  const auto layout = ComputeLayout(ultrawide ? uint32_t(dimensions>>32) : 1280,
      ultrawide ? uint32_t(dimensions) : 720,
      custom ? REXCVAR_GET(forza_hud_safe_width) : 100.0,
      custom ? REXCVAR_GET(forza_hud_safe_height) : 100.0,
      custom ? REXCVAR_GET(forza_hud_offset_x) : 0.0,
      custom ? REXCVAR_GET(forza_hud_offset_y) : 0.0, REXCVAR_GET(forza_hud_edges));
  const bool was = wide.load();
  const bool active = GameplayLayoutActive(layout.aspect_scale, was, n, game, map);
  aspect_scale = layout.aspect_scale;
  wide = active;
  // Callback coalesces unchanged values before dispatching to the UI thread.
  present(!(active && ultrawide && layout.aspect_scale > 1.0), active ? layout.hud_scale_x : 1.0, active ? layout.hud_scale_y : 1.0,
          active ? layout.hud_offset_x : 0.0, active ? layout.hud_offset_y : 0.0, active ? layout.hud_edge_shift : 0.0);
  if (was != active) REXLOG_INFO("Ultrawide: {}", active ? "gameplay" : "protected 16:9 screen");
}
double Projection(bool gameplay, bool map, uint32_t width, uint32_t height, double aspect) {
  if (!ready.load() || width != 1280 || height != 720) return aspect;
  if (map) { map_frame = frame.load(); return aspect; }
  if (gameplay) gameplay_frame = frame.load();
  return wide.load() ? aspect * aspect_scale.load() : aspect;
}
}
