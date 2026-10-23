// A stopped game or a sleeping machine (src/platform/resume_gap.h, #140): the watcher's ticks
// find each, and a busy machine is neither. Build from the repository root:
//   g++ -std=c++20 -Wall -Wextra -Werror -Iforza-recomp/src forza-recomp/tests/resume_gap_test.cpp
//       -o resume-gap-test && ./resume-gap-test
#include "platform/resume_gap.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using forza::resume::Gap;
using forza::resume::GapDetector;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// Ticks every 100 ms from `t` for `seconds`; true if any of them found a gap.
bool Run(GapDetector& d, double& t, double asleep, double seconds) {
  bool found = false;
  for (const double end = t + seconds; t < end; t += 0.1) {
    const Gap g = d.Tick(t, asleep);
    found |= g.stopped > 0 || g.suspended > 0;
  }
  return found;
}
}  // namespace

int main() {
  {  // running normally, and the first tick (the time since boot is no gap)
    GapDetector d;
    double t = 5000;
    Check(!Run(d, t, 3600, 60), "a minute of ticks");
  }
  {  // the process stopped for 3 s, then for 10 minutes
    GapDetector d;
    double t = 10;
    Run(d, t, 0, 1);
    t += 2.9;
    Gap g = d.Tick(t, 0);
    Check(Near(g.stopped, 3.0) && g.suspended == 0, "a 3 s stop");
    Check(!Run(d, t, 0, 5), "ticks after it are normal");
    t += 600;
    g = d.Tick(t, 0);
    Check(g.stopped > 600 && g.suspended == 0, "a 10 minute stop");
  }
  {  // a gap under a second is a busy machine, not a stop
    GapDetector d;
    Check(d.Tick(1.0, 0).stopped == 0, "first tick");
    Check(d.Tick(1.95, 0).stopped == 0, "950 ms");
    Check(Near(d.Tick(3.05, 0).stopped, 1.1), "1.1 s");
  }
  {  // a system suspend: the running clock stops, the sleep time grows
    GapDetector d;
    double t = 100;
    Run(d, t, 50, 1);
    const Gap g = d.Tick(t, 50 + 7200);
    Check(Near(g.suspended, 7200) && g.stopped == 0, "two hours asleep");
    Check(!Run(d, t, 50 + 7200, 5), "ticks after it are normal");
  }
  {  // the game paused before the machine slept: both, counted apart
    GapDetector d;
    Check(d.Tick(1, 0).stopped == 0, "first tick");
    const Gap g = d.Tick(4, 600);
    Check(Near(g.stopped, 3) && Near(g.suspended, 600), "a 3 s stop around 600 s asleep");
  }
  std::puts("resume gap: OK");
  return 0;
}
