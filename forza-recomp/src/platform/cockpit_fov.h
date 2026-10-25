#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>

namespace forza::camera {
// Same world object and fields already sampled by hooks/telemetry.cpp for wheel
// replay/rewind suppression. Record its address before that update calls the
// camera readers, then inspect current guest flags without a debounce delay.
inline std::atomic<uint32_t> cockpit_world{0};
inline bool LiveCockpitWorld(const uint8_t* world) {
  if (!world || world[0x0C] || !world[0x0D]) return false;
  const auto* p = world + 0x28;
  const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float scale;
  std::memcpy(&scale, &bits, 4);
  return scale > 0.0f;
}
// Zero means untouched game FOV, including camera transitions and effects.
constexpr bool ValidCockpitFov(int degrees) {
  return degrees == 0 || (degrees >= 40 && degrees <= 90);
}

// The director stores vertical radians in big-endian guest memory. Both its
// presentation and visibility readers must see the same value. Never feed the
// override back into camera simulation or replace a newer game update on exit.
class ScopedCockpitFov {
 public:
  ScopedCockpitFov(uint8_t* fov, int degrees, bool driver, bool vr_active) {
    if (!fov || !driver || vr_active || degrees == 0 || !ValidCockpitFov(degrees)) return;
    field_ = fov;
    std::memcpy(original_, field_, 4);
    const float radians = float(degrees) * (3.14159265358979323846f / 180.0f);
    uint32_t bits;
    std::memcpy(&bits, &radians, 4);
    replacement_[0] = uint8_t(bits >> 24);
    replacement_[1] = uint8_t(bits >> 16);
    replacement_[2] = uint8_t(bits >> 8);
    replacement_[3] = uint8_t(bits);
    std::memcpy(field_, replacement_, 4);
  }
  ~ScopedCockpitFov() {
    if (field_ && std::memcmp(field_, replacement_, 4) == 0)
      std::memcpy(field_, original_, 4);
  }
  ScopedCockpitFov(const ScopedCockpitFov&) = delete;
  ScopedCockpitFov& operator=(const ScopedCockpitFov&) = delete;

 private:
  uint8_t* field_ = nullptr;
  uint8_t original_[4]{};
  uint8_t replacement_[4]{};
};
}  // namespace forza::camera
