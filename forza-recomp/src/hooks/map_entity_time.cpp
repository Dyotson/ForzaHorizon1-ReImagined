// forza - the map entities' frame time below 30 fps (docs/framerate.md, #263).
//
// CMapComponent's update (vtable slot 14, sub_82611590) hands the frame time to two lists of
// map entities: sub_8255DE80 steps the activities (CMapEntityActivity), sub_825581E8 the
// barn-find regions (CMapEntityBarnFindRegion), each entity through its vtable +0x30. Both
// clamp the frame time to 1/30 s first (the console never runs slower on purpose), so at
// 20 fps (50 ms frames) the entities moved at two thirds of their speed. At rates other than
// the console's 30, a frame longer than 1/30 s is handed to them in two halves: up to 1/15 s
// (a frame of a 15 fps game) arrives whole, longer stalls are clamped as before. At 30 the
// game's own code runs.

#include "frame_time.h"
#include "../platform/guest_addr.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace {

constexpr double kLongestStep = double(1.0f / 30.0f);  // the float 1/30 the game clamps to

using GuestFunc = void (*)(PPCContext&, uint8_t*);

void StepInHalves(PPCContext& ctx, uint8_t* base, GuestFunc original) {
  const double dt = ctx.f1.f64;
  if (forza::frame_time::ConsoleRate() || !(dt > kLongestStep)) {
    original(ctx, base);
    return;
  }
  const uint64_t self = ctx.r3.u64;
  const double half = double(float(dt * 0.5));
  ctx.f1.f64 = half;
  original(ctx, base);
  ctx.r3.u64 = self;
  ctx.f1.f64 = half;
  original(ctx, base);
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_8255DE80);
extern "C" REX_FUNC(sub_8255DE80) { StepInHalves(ctx, base, __imp__sub_8255DE80); }

extern "C" REX_FUNC(__imp__sub_825581E8);
extern "C" REX_FUNC(sub_825581E8) { StepInHalves(ctx, base, __imp__sub_825581E8); }
