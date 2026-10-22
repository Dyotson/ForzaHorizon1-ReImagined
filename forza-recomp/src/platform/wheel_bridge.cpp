// forza - the racing wheel's force feedback stops while the player is not driving (#256).
//
// The game keeps its wheel effects going when the car is not under the player's control: in the
// pause menu, a rewind and a replay its centring spring and damper keep their last strength, and a
// rewind plays the steering force and rumble of the car going backwards, so the wheel pulled and
// buzzed (a G923 owner, #4). The SDK's wheel driver (patch 0101) fades the game's forces out while
// the player is not driving and back in when they are; it learns which from
// ForzaWheelPlayerDriving, an export of the runtime found at run time (the stock SDK headers do not
// have it; an older runtime just lacks it). "Driving" is the car's physics stepping
// (driving_gate.h IsSimulating): the substeps stop in all of those states. The rumble gate
// (IsDriving) cannot tell: with a wheel the game sends no XamInputSetState rumble at all.
// FORZA_WHEEL_DRIVING_GATE=0 never reports, so the forces never fade.

#include "wheel_bridge.h"

#include "driving_gate.h"
#include "sdk_import.h"

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
  static std::atomic<int> reported{-1};
  const int driving = telemetry::IsSimulating() ? 1 : 0;
  if (reported.exchange(driving, std::memory_order_relaxed) != driving) report(driving);
}

}  // namespace forza::wheel
