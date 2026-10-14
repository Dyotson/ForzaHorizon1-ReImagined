// forza - crowd animation speed above 30 fps (docs/framerate.md, #7).
//
// The skinned spectators (PROC_CHARACTER_SKIN*) play animations baked at 30 frames a
// second (sub_82DDD6A8 samples each Granny clip every 1/30 s). Each render of a view,
// sub_824788B0 hands the crowd object the simulated time since the view's last render
// (+0x1DE8) through sub_82DF7BB0, which sets the number of baked frames to advance
// (object +0x3EAE4). That function keeps a microsecond accumulator (+0x3EAE0, 25 ms
// frames, from the sprite crowd) but then overwrites the step with 1 whenever time
// passed. sub_82E08A00 adds the step to every spectator's frame (and picks a new clip at
// the end). So the crowd advances one baked frame per rendered frame: right at the
// console's 30 fps, 2x at 60 and about 3.5x at 144 (the intro renders about 100 fps).
//
// Above 30 fps the step is the number of 1/30 s periods the simulated time crossed: the
// accumulator (in 1/3 us, so a period is exactly 100000) keeps the remainder. At 30 the
// game's own function runs; FORZA_CROWD_ANIM=game keeps it at every rate (comparisons).

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

REXCVAR_DECLARE(int32_t, frame_rate);  // src/main.cpp

namespace {

constexpr uint32_t kAccumulator = 0x3EAE0;  // crowd object: int, 1/3 us above 30 fps
constexpr uint32_t kFrameStep = 0x3EAE4;    // crowd object: baked frames to advance
constexpr double kUnitsPerSecond = 3e6;
constexpr int32_t kUnitsPerFrame = 100000;  // 1/30 s

int32_t Load32(const uint8_t* p) {
  return int32_t(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
}

void Store32(uint8_t* p, int32_t v) {
  const uint32_t u = uint32_t(v);
  p[0] = uint8_t(u >> 24);
  p[1] = uint8_t(u >> 16);
  p[2] = uint8_t(u >> 8);
  p[3] = uint8_t(u);
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_82DF7BB0);
extern "C" REX_FUNC(sub_82DF7BB0) {
  static const bool game = [] {
    const char* v = std::getenv("FORZA_CROWD_ANIM");
    return v && std::strcmp(v, "game") == 0;
  }();
  if (game || REXCVAR_GET(frame_rate) <= 30) {
    __imp__sub_82DF7BB0(ctx, base);
    return;
  }
  uint8_t* acc = forza::GuestPtr(base, ctx.r3.u32 + kAccumulator);
  uint8_t* step = forza::GuestPtr(base, ctx.r3.u32 + kFrameStep);
  const double dt = ctx.f1.f64;
  if (!(dt > 0.0)) {  // as the game: no time, no step
    Store32(step, 0);
    return;
  }
  // The game's 25 ms remainder (frame rate changed at run time) can exceed a period: fine.
  int64_t units = int64_t(Load32(acc)) + std::llround(std::min(dt, 60.0) * kUnitsPerSecond);
  if (units < 0) units = 0;
  Store32(step, int32_t(units / kUnitsPerFrame));
  Store32(acc, int32_t(units % kUnitsPerFrame));
}
