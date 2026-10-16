// Built-in ultrawide layout math; no runtime or plugin dependencies.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace forza::ultrawide {
constexpr double kNativeAspect = 16.0 / 9.0;
struct Layout { double aspect_scale, hud_scale_x, hud_scale_y, hud_offset_x, hud_offset_y, hud_edge_shift; };
inline Layout ComputeLayout(uint32_t width, uint32_t height, double safe_width,
                            double safe_height, double offset_x, double offset_y, bool edges = false) {
  const double ratio = width && height ? double(width) / height : kNativeAspect;
  const double aspect = std::clamp(ratio / kNativeAspect, 1.0, 4.0);
  const double fit = std::clamp(std::min(safe_width, safe_height), 50.0, 100.0) / 100.0;
  // Fit a 16:9 HUD rectangle into the requested safe area without stretching it.
  const double sx = fit / aspect, sy = fit;
  const double extent_x = edges ? fit : sx;
  return {aspect, sx, sy, std::clamp(offset_x / 50.0, -(1.0-extent_x), 1.0-extent_x),
          std::clamp(offset_y / 50.0, -(1.0-sy), 1.0-sy), edges ? fit-sx : 0.0};
}
inline bool GameplayLayoutActive(double aspect, bool was_wide, uint64_t now,
                         uint64_t game, uint64_t map) {
  if (aspect < 1.0 || !game || (map && now-map <= 3)) return false;
  return now-game <= (was_wide ? 30u : 2u);
}
inline uint32_t RenderScale(uint32_t extent, uint32_t guest_extent, uint32_t requested) {
  return std::clamp(std::max(requested, (extent + guest_extent - 1) / guest_extent), 1u, 7u);
}
}  // namespace forza::ultrawide
