#include "../src/platform/auto_pause_policy.h"
#include <cassert>
#include <cstdio>
using forza::auto_pause::Policy;
int main() {
  Policy p;
  assert(!p.Poll(0, 4, 8, true, true, true, false));  // initial observations are not losses
  assert(p.Poll(10, 5, 8, true, true, true, false));
  assert(p.Poll(100, 5, 8, true, true, true, false));
  assert(!p.Poll(160, 5, 8, true, true, true, false));
  assert(!p.Poll(200, 5, 8, true, true, true, false));  // regaining focus is not a resume
  assert(p.Poll(210, 5, 9, true, true, true, false));
  assert(!p.Poll(220, 5, 9, true, true, false, false));  // already paused: cancel
  assert(!p.Poll(240, 5, 9, true, true, true, false));
  assert(!p.Poll(250, 6, 10, true, true, false, false));  // menus/loading/rewind: consume events
  assert(!p.Poll(270, 6, 10, true, true, true, false));
  assert(p.Poll(280, 7, 10, true, true, true, false));
  assert(!p.Poll(290, 7, 10, false, true, true, false));  // disabling its option cancels a pulse
  assert(!p.Poll(300, 8, 10, false, true, true, false));
  assert(p.Poll(310, 8, 11, false, true, true, false));
  p.Cancel();  // overlay/benchmark owns input
  assert(!p.Poll(315, 8, 11, false, true, true, false));
  assert(!p.Poll(320, 9, 12, true, true, true, true));  // player's START wins
  assert(!p.Poll(330, 9, 12, true, true, true, false));
  Policy legacy;
  assert(!legacy.Poll(0, 0, std::nullopt, true, true, true, false));
  assert(legacy.Poll(1, 1, std::nullopt, true, true, true, false));  // focus works on an old SDK
  assert(!legacy.Poll(2, 1, 0xffffffffu, false, true, true, false));
  assert(legacy.Poll(3, 1, 0u, false, true, true, false));  // sequence wrap remains an edge
  std::puts("auto-pause policy: focus/disconnect edges, guards, release and legacy SDK pass");
}
