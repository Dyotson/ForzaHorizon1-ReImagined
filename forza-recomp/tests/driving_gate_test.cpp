// The trigger effects' "player is driving" gate (src/platform/driving_gate.h, #224): the game's
// rumble turns it on, a hold bridges quiet moments, and it releases in a menu. Build from the
// repository root:
//   g++ -std=c++23 -Wall -Wextra -Werror -Iforza-recomp/src forza-recomp/tests/driving_gate_test.cpp
//       -o driving-gate-test && ./driving-gate-test
#include "platform/driving_gate.h"

#include <cstdio>
#include <cstdlib>

using forza::telemetry::DrivingGate;
using forza::telemetry::kDrivingHoldMs;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}
}  // namespace

int main() {
  DrivingGate gate;
  Check(!gate.Driving(1000), "not driving before any rumble");
  gate.Rumble(0, 0, 1000);
  Check(!gate.Driving(1000), "zero rumble (menus send it) does not count");
  gate.Rumble(0, 500, 2000);
  Check(gate.Driving(2000), "rumble means driving");
  gate.Rumble(0, 0, 2500);
  Check(gate.Driving(2500 + 100), "zero rumble does not cancel the hold");
  Check(gate.Driving(2000 + kDrivingHoldMs - 1), "held through a quiet moment");
  Check(!gate.Driving(2000 + kDrivingHoldMs), "released once the hold ends (menu)");
  gate.Rumble(300, 0, 9000);
  Check(gate.Driving(9500), "comes back when driving resumes");
  DrivingGate fresh;
  fresh.Rumble(1, 1, 0);
  Check(fresh.Driving(10), "time zero is a valid rumble time");
  forza::telemetry::Gate().Rumble(1, 0, forza::telemetry::DrivingNowMs());
  Check(forza::telemetry::IsDriving(), "the shared flag follows the rumble");
  std::puts("PASS");
}
