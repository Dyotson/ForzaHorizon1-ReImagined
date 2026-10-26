// forza - the VR cockpit's cabin drawn from where the eye really is (#333, docs/vr.md "Version 8").
//
// The render thread draws the player's car interior in its own pass, sub_82DCF1C0 (renderer, view
// constants): the pass's coordinates have their origin at the car's in-car camera point, which the
// car render object (sub_82447400 of the renderer's +4) holds at +1872 as x y z w in signed fixed
// point with 18 fraction bits. The pass takes the camera's world position from the view constants
// (+192), keeps it at +208, and computes the camera's offset from that point; when the offset is
// shorter than 10 cm (its square under the constant 0.01) it uses zero. It then writes the offset
// as the camera's position (+192), as the translation of the camera-to-world matrix (+128, row 3 at
// +176) and inverts that into the view (+0), draws the cabin with them (sub_823F37D0 and twice
// sub_82447CA8, each with the view constants in r5) and puts the constants back.
//
// So inside 10 cm the cabin is drawn as if the eye were exactly at the in-car camera's point: a
// head moved 6 cm sees the dashboard, the wheel and the pillars where it saw them before, and as the
// light and the shadows on them are computed from where the eye really is (the world's constants),
// they slide over the cabin when the head moves or turns. On a monitor the snap keeps the cabin still
// under the camera's shake; past 10 cm the game uses the real offset, which is the jump version 7
// found.
//
// In the VR cockpit the hooks below give the cabin passes the real offset at every distance:
// sub_82DCF1C0 marks its thread as inside the pass, sub_82447400 hands over the car render object it
// returns there, and before each of the three cabin draws the offset, the camera's position, its
// matrix's translation and the view are written again from the position the pass saved and the
// car's point, without the 10 cm test (vr_cockpit_space.h). sub_82DCF1C0 puts the constants back
// itself after the draws.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include "../platform/vr_cockpit_space.h"
#include "../platform/xr.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(vr_cockpit_parallax, true, "VR",
                    "Cockpit: draw the car's interior from where the head really is (the game draws it "
                    "from the in-car camera's point while the view is within 10 cm of it, so the cabin "
                    "and its shadows followed the head)");

namespace {
constexpr uint32_t kViewMatrix = 0;          // view constants: world to camera
constexpr uint32_t kCameraMatrix = 128;      // camera to world (row 3 at +176: the position)
constexpr uint32_t kCameraPosition = 192;    // the camera's position (in the pass: the offset)
constexpr uint32_t kSavedPosition = 208;     // the pass keeps the world position here
constexpr uint32_t kCarCameraPoint = 1872;   // car render object: the in-car camera point, fixed point

struct Pass {
  bool active = false;
  uint32_t constants = 0;
  uint32_t car = 0;
};
thread_local Pass pass;
std::atomic<int> parallax_test{-1};  // a FIFO's "parallax 0|1" (vr_head_pose.cpp); -1: the cvar

bool Valid(uint32_t address) { return address >= 0x00100000 && address < 0xF0000000; }

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float LoadFloat(uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void StoreFloat(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  uint8_t* p = forza::GuestPtr(base, address);
  p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8), p[3] = uint8_t(bits);
}

bool On() {
  if (!forza::xr::CockpitActive()) return false;
  const int test = parallax_test.load(std::memory_order_relaxed);
  return test >= 0 ? test >= 1 : REXCVAR_GET(vr_cockpit_parallax);
}

// Before a cabin draw of the pass: the camera's real offset from the car's in-car camera point.
void Unsnap(uint8_t* base, uint32_t constants) {
  if (!pass.active || constants != pass.constants || !Valid(pass.car)) return;
  float camera[4];
  int32_t point[4];
  for (int i = 0; i < 4; ++i) {
    camera[i] = LoadFloat(base, constants + kSavedPosition + 4 * i);
    point[i] = int32_t(Load32(base, pass.car + kCarCameraPoint + 4 * i));
  }
  float offset[4];
  forza::vr::CockpitOffset(camera, point, offset);
  // Sanity: the in-car camera point is the car's, a few metres at most from the camera.
  if (!(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] < 100.0f)) return;
  // (A test aid, FIFO "parallax 2": the shading keeps the game's eye position, only the geometry
  // moves, to tell view-dependent shading from light that slides.)
  if (parallax_test.load(std::memory_order_relaxed) != 2)
    for (int i = 0; i < 4; ++i) StoreFloat(base, constants + kCameraPosition + 4 * i, offset[i]);
  float m[16];
  for (int i = 0; i < 12; ++i) m[i] = LoadFloat(base, constants + kCameraMatrix + 4 * i);
  m[12] = offset[0], m[13] = offset[1], m[14] = offset[2], m[15] = 1.0f;
  for (int i = 12; i < 16; ++i) StoreFloat(base, constants + kCameraMatrix + 4 * i, m[i]);
  float view[16];
  forza::vr::RigidInverse(m, view);
  for (int i = 0; i < 16; ++i) StoreFloat(base, constants + kViewMatrix + 4 * i, view[i]);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true))
    REXLOG_INFO("VR: the cabin is drawn from the eye's own position ({:.1f} cm from the in-car camera's "
                "point; the game would use 0 within 10 cm)",
                100.0f * std::sqrt(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]));
}
}  // namespace

namespace forza::xr {
void SetCockpitParallaxTest(int on) { parallax_test.store(on, std::memory_order_relaxed); }
}  // namespace forza::xr

// The cabin pass (renderer, view constants, ...).
extern "C" REX_FUNC(__imp__sub_82DCF1C0);
extern "C" REX_FUNC(sub_82DCF1C0) {
  if (!On()) {
    __imp__sub_82DCF1C0(ctx, base);
    return;
  }
  const Pass outer = pass;
  pass = {true, ctx.r4.u32, 0};
  __imp__sub_82DCF1C0(ctx, base);
  pass = outer;
}

// The renderer's car render object (the camera's focused car); the cabin pass asks for it first.
extern "C" REX_FUNC(__imp__sub_82447400);
extern "C" REX_FUNC(sub_82447400) {
  __imp__sub_82447400(ctx, base);
  if (pass.active && !pass.car) pass.car = ctx.r3.u32;
}

// The cabin pass's draws (renderer, ?, view constants).
extern "C" REX_FUNC(__imp__sub_823F37D0);
extern "C" REX_FUNC(sub_823F37D0) {
  Unsnap(base, ctx.r5.u32);
  __imp__sub_823F37D0(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_82447CA8);
extern "C" REX_FUNC(sub_82447CA8) {
  Unsnap(base, ctx.r5.u32);
  __imp__sub_82447CA8(ctx, base);
}
