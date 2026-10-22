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

// "The car's physics is running" (#256): the physics substep hook (hooks/telemetry.cpp) steps it.
// The substeps stop in the pause menu, rewinds, replays, menus and loading screens, and unlike the
// rumble above this needs nothing from the pad: the game sends a racing wheel's rumble through
// XInputdFFSetRumble, never XamInputSetState, so the rumble gate never opens with a wheel. A gap of
// kSimulationHoldMs ends a run (substeps come at 60 a second); a run counts once it has lasted
// kSimulationSettleMs, which leaves out the bursts of substeps when a replay starts (1.3 s from
// the rewind menu's View replay, 0.3 s at its Restart), mostly. Read it with IsSimulating().
constexpr uint64_t kSimulationHoldMs = 250;
constexpr uint64_t kSimulationSettleMs = 500;
struct SimulationGate {
  std::atomic<uint64_t> last_ms{0};   // the last substep; 0 = never
  std::atomic<uint64_t> since_ms{0};  // the first substep of the current run
  void Step(uint64_t now) {
    now = now ? now : 1;
    const uint64_t last = last_ms.load(std::memory_order_relaxed);
    if (!last || now - last >= kSimulationHoldMs) since_ms.store(now, std::memory_order_relaxed);
    last_ms.store(now, std::memory_order_relaxed);
  }
  bool Running(uint64_t now) const {
    const uint64_t last = last_ms.load(std::memory_order_relaxed);
    const uint64_t since = since_ms.load(std::memory_order_relaxed);
    return last && now >= last && now - last < kSimulationHoldMs && now - since >= kSimulationSettleMs;
  }
};
inline SimulationGate& Simulation() { static SimulationGate gate; return gate; }
inline bool IsSimulating() { return Simulation().Running(DrivingNowMs()); }
}
