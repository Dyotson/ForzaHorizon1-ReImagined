// Standalone check of the VR cockpit's cabin offset maths (src/platform/vr_cockpit_space.h).
// c++ -std=c++20 -Wall -Wextra -Werror -I forza-recomp/src forza-recomp/tests/vr_cockpit_space_test.cpp -o /tmp/vr_cockpit_space_test && /tmp/vr_cockpit_space_test
#include "platform/vr_cockpit_space.h"

#include <cmath>
#include <cstdio>

namespace {
using namespace forza::vr;
int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

void TestFixedPoint() {
  Check(ToFixed18(1.0f) == 262144, "1.0 is 2^18");
  Check(ToFixed18(-1.0f) == -262144, "-1.0 is -2^18");
  Check(ToFixed18(1.9999999f / 262144.0f) == 1, "rounded toward zero");
  Check(ToFixed18(-1.9999999f / 262144.0f) == -1, "rounded toward zero below zero");
  Check(ToFixed18(1e9f) == 2147483647, "saturated high");
  Check(ToFixed18(-1e9f) == -2147483647 - 1, "saturated low");
  Check(ToFixed18(std::nanf("")) == 0, "NaN is 0");
  Check(SubSat(2147483647, -1) == 2147483647, "subtraction saturates");
}

// The offset the game computes: the camera's position minus the point, exact to 1/2^18 m far from
// the origin (the world is a few km across), where a float subtraction would lose millimetres.
void TestOffset() {
  const float point_m[4] = {87.9843f, 101.6482f, -3920.9001f, 1.0f};
  int32_t point[4];
  for (int i = 0; i < 4; ++i) point[i] = ToFixed18(point_m[i]);
  const float camera[4] = {87.9843f + 0.06f, 101.6482f - 0.04f, -3920.9001f + 0.03f, 1.0f};
  float off[4];
  CockpitOffset(camera, point, off);
  Check(Near(off[0], 0.06f, 2e-4f) && Near(off[1], -0.04f, 2e-4f) && Near(off[2], 0.03f, 5e-4f),
        "offset is camera minus point");
  Check(Near(off[3], 0.0f), "w cancels");
  Check(GameSnaps(off), "the game snaps 7.8 cm to zero");
  const float far_camera[4] = {87.9843f + 0.12f, 101.6482f, -3920.9001f, 1.0f};
  CockpitOffset(far_camera, point, off);
  Check(!GameSnaps(off), "the game keeps 12 cm");
  // Edge of the game's sphere: 9.9 cm snaps, 10.1 cm does not (measured 9.9-10.1 cm in v7).
  const float in[4] = {87.9843f, 101.6482f - 0.099f, -3920.9001f, 1.0f};
  const float out[4] = {87.9843f, 101.6482f - 0.101f, -3920.9001f, 1.0f};
  CockpitOffset(in, point, off);
  Check(GameSnaps(off), "9.9 cm snaps");
  CockpitOffset(out, point, off);
  Check(!GameSnaps(off), "10.1 cm does not");
}

void TestRigidInverse() {
  // A camera turned 30 degrees about y at (0.06, -0.04, 0.03): the view takes its position to the
  // origin and the axes back to the identity.
  const float c = std::cos(0.5236f), s = std::sin(0.5236f);
  const float m[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0.06f, -0.04f, 0.03f, 1};
  float v[16];
  RigidInverse(m, v);
  float p[4] = {0, 0, 0, 0};
  const float pos[4] = {0.06f, -0.04f, 0.03f, 1};
  for (int j = 0; j < 4; ++j)
    for (int k = 0; k < 4; ++k) p[j] += pos[k] * v[4 * k + j];
  Check(Near(p[0], 0) && Near(p[1], 0) && Near(p[2], 0) && Near(p[3], 1), "the camera's position goes to the origin");
  float prod[16] = {};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) prod[4 * i + j] += m[4 * i + k] * v[4 * k + j];
  bool identity = true;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) identity &= Near(prod[4 * i + j], i == j ? 1.0f : 0.0f);
  Check(identity, "camera matrix times view is the identity");
}
}  // namespace

int main() {
  TestFixedPoint();
  TestOffset();
  TestRigidInverse();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("vr_cockpit_space_test: all passed\n");
  return 0;
}
