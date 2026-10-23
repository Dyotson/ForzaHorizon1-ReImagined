// forza - the racing wheel's force feedback stops while the player is not driving (#256, #304).
//
// The game keeps its wheel effects going when the car is not under the player's control: in the
// pause menu, a rewind and a replay its centring spring and damper keep their last strength, and a
// rewind plays the steering force and rumble of the car going backwards, so the wheel pulled and
// buzzed (a G923 owner, #4). The SDK's wheel driver (patch 0101) fades the game's forces out while
// the player is not driving and back in when they are; it learns which from
// ForzaWheelPlayerDriving, an export of the runtime found at run time (the stock SDK headers do not
// have it; an older runtime just lacks it). "Driving" comes from the game's own simulation state,
// read once a frame (driving_gate.h PlayerGate): its pause flag, its time scale (a rewind runs it
// backwards), its started flag and the car's physics, and the pad polls for menus. It is counted in
// frames: a hitch, a wall hit or the window losing focus never fades the forces (#304: the gate
// before ended a run after 250 ms without a physics substep, and every hitch that long took the
// forces away for a second). Each change is logged with its reason. The rumble gate (IsDriving)
// cannot tell: with a wheel the game sends no XamInputSetState rumble at all.
// FORZA_WHEEL_DRIVING_GATE=0 never reports, so the forces never fade.

#include "wheel_bridge.h"

#include "driving_gate.h"
#include "sdk_import.h"

#include <rex/logging.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace forza::wheel {

void UpdateDriving() {
  using Report = void (*)(int);
  static const Report report = [] {
    const char* gate = std::getenv("FORZA_WHEEL_DRIVING_GATE");
    if (gate && std::strcmp(gate, "0") == 0) return Report{};
    return reinterpret_cast<Report>(SdkSymbol("ForzaWheelPlayerDriving"));
  }();
  if (!report) return;
  auto& gate = telemetry::Player();
  gate.Poll(telemetry::DrivingNowMs());
  const telemetry::Drive state = gate.State();
  if (state == telemetry::Drive::kUnknown) return;
  static std::atomic<telemetry::Drive> logged{telemetry::Drive::kUnknown};
  const telemetry::Drive before = logged.exchange(state, std::memory_order_relaxed);
  if (before == state) return;
  if (state == telemetry::Drive::kDriving) {
    REXLOG_INFO("Wheel: driving: the game's forces fade back in");
  } else if (before == telemetry::Drive::kDriving || before == telemetry::Drive::kUnknown) {
    REXLOG_INFO("Wheel: not driving ({}): the game's forces fade out", telemetry::DriveName(state));
  } else {
    REXLOG_INFO("Wheel: still not driving ({})", telemetry::DriveName(state));
  }
  static std::atomic<int> reported{-1};
  const int driving = state == telemetry::Drive::kDriving ? 1 : 0;
  if (reported.exchange(driving, std::memory_order_relaxed) != driving) report(driving);
}

}  // namespace forza::wheel
