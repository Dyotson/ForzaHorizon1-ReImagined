#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace forza::telemetry {
// "The player is driving" (#224), shared by the trigger effects and anything else that must
// stop in menus, pause and replays (wheel force feedback, #256). The car stays in the game's
// vehicle list in menus (garage, showroom), so a local car and fresh input do not tell a menu
// from the road. The game's own rumble does: it sets the pad's motors (XamInputSetState) while
// the car runs, and not in menus or pause. Quiet moments (an idle engine on smooth tarmac) are
// bridged by a hold. Read it with forza::telemetry::IsDriving().
constexpr uint64_t kDrivingHoldMs = 3000;
struct DrivingGate {
  std::atomic<uint64_t> rumble_ms{0};  // last time the game asked for a non-zero rumble; 0 = never
  void Rumble(uint16_t left, uint16_t right, uint64_t now) {
    if (left || right) rumble_ms.store(now ? now : 1, std::memory_order_relaxed);
  }
  bool Driving(uint64_t now) const {
    const uint64_t last = rumble_ms.load(std::memory_order_relaxed);
    return last && now - last < kDrivingHoldMs;
  }
};
inline uint64_t DrivingNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline DrivingGate& Gate() { static DrivingGate gate; return gate; }
// Thread safe; no telemetry bridge needed. Fed by the XamInputSetState hook.
inline bool IsDriving() { return Gate().Driving(DrivingNowMs()); }
}
