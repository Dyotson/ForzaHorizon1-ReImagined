// The trigger effects' "player is driving" gate (src/platform/driving_gate.h, #224): the game's
// rumble turns it on, a hold bridges quiet moments, and it releases in a menu. And the racing
// wheel's (#256): the physics substeps, which stop in the pause menu, rewinds and replays. Build from the
// repository root:
//   g++ -std=c++23 -Wall -Wextra -Werror -Iforza-recomp/src forza-recomp/tests/driving_gate_test.cpp
//       -o driving-gate-test && ./driving-gate-test
#include "platform/driving_gate.h"

#include <cstdio>
#include <cstdlib>

using forza::telemetry::DrivingGate;
using forza::telemetry::kDrivingHoldMs;
using forza::telemetry::kSimulationHoldMs;
using forza::telemetry::kSimulationSettleMs;
using forza::telemetry::SimulationGate;

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

  // The physics gate: substeps at 60 a second (one every 16 ms) are driving once they have lasted
  // the settle time; a gap of the hold time (pause, rewind, replay) ends it at once.
  SimulationGate sim;
  Check(!sim.Running(1000), "no substep yet: not driving");
  uint64_t t = 1000;
  for (; t < 1000 + kSimulationSettleMs; t += 16) {
    sim.Step(t);
    Check(!sim.Running(t), "a run shorter than the settle time is not driving yet");
  }
  sim.Step(t);
  Check(sim.Running(t), "a run past the settle time is driving");
  Check(sim.Running(t + kSimulationHoldMs - 1), "a hitch shorter than the hold keeps driving");
  Check(!sim.Running(t + kSimulationHoldMs), "no substep for the hold time: the pause or a rewind");
  // A burst of substeps as a replay starts (0.3 s) does not count as driving.
  uint64_t burst = t + 5000;
  for (uint64_t u = burst; u < burst + 300; u += 16) {
    sim.Step(u);
    Check(!sim.Running(u), "a short burst while a replay starts is not driving");
  }
  // Driving resumes: the settle time again, then driving.
  uint64_t resume = burst + 2000;
  for (uint64_t u = resume; u <= resume + kSimulationSettleMs + 16; u += 16) sim.Step(u);
  Check(sim.Running(resume + kSimulationSettleMs + 16), "driving again after the settle time");
  // A burst of thousands of substeps in one frame (seen while driving) changes nothing.
  for (int i = 0; i < 5000; ++i) sim.Step(resume + kSimulationSettleMs + 20);
  Check(sim.Running(resume + kSimulationSettleMs + 30), "a burst keeps driving");
  Check(!sim.Running(resume), "a time before the last substep (another thread) is no driving");
  SimulationGate zero;
  zero.Step(0);
  Check(!zero.Running(0) && zero.last_ms.load() == 1, "time zero is a valid substep time");
  forza::telemetry::Simulation().Step(forza::telemetry::DrivingNowMs());
  Check(!forza::telemetry::IsSimulating(), "the shared flag waits for the settle time");
  std::puts("PASS");
}
