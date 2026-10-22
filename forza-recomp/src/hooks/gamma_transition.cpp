// forza - the screen gamma transition ends when it reaches its target (#259, docs/perf.md).
//
// The render thread begins each pass of its loop with the gamma transition (sub_82586CD8,
// TU4 sub_825FCD50). The game starts one by setting a channel's target and the start time
// (render object +0x16E0, sub_82586C28, TU4 sub_825FCCA0). While the start time is above 0,
// each pass computes the progress (now - start) / duration (+0x16E8), takes at most 1 of
// it, lerps the three gamma parameters, and builds and sets the gamma ramp
// (sub_82586BB0 > vtable +200 sub_82C28070, TU4 sub_825FCC28 > sub_82CE8948: 256 calls of
// the CRT's pow). It then clears the start time when that progress is above 1. As it never
// is, the transition never ends: after the game's first gamma change every pass rebuilds
// and sets the same ramp: 0.5 ms of the render thread's CPU a frame in free roam on Linux
// (where #234's idle wait already cut the passes to 730 a second), and much more where the
// thread spins between frames (Windows before #259).
//
// Now, after a pass has built the ramp at progress 1, its start time is cleared, as the game
// does for progress above 1: the last ramp set is the one every later pass would have set
// again. A new transition (the game's next gamma change) runs as before. Only that call of
// sub_82586BB0 is touched (told by its return address); the game also calls it when the
// render device is made and when a gamma change applies at once.
// FORZA_GAMMA_TRANSITION=game keeps the game's endless transition.

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kTransitionReturn = FORZA_ADDR(0x82586D80);  // sub_82586CD8's call
constexpr uint32_t kGammaStart = 0x16E0;  // double: the transition's start time; 0 = none

bool GameTransition() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_GAMMA_TRANSITION");
    return v && std::strcmp(v, "game") == 0;
  }();
  return on;
}

}  // namespace

// Sets the gamma ramp from the three gamma parameters (f1-f3) of render object r3.
extern "C" REX_FUNC(__imp__sub_82586BB0);
extern "C" REX_FUNC(sub_82586BB0) {
  // In the transition, f30 holds the progress it applies, already at most 1.
  const bool done = ctx.lr == kTransitionReturn && ctx.f30.f64 >= 1.0 && !GameTransition();
  const uint32_t render = ctx.r3.u32;
  __imp__sub_82586BB0(ctx, base);
  if (done) std::memset(forza::GuestPtr(base, render + kGammaStart), 0, 8);  // +0.0
}
