// Standalone check of the OpenXR pose and field-of-view maths (src/platform/xr_math.h).
// c++ -std=c++20 -Wall -Wextra -Werror -I forza-recomp/src forza-recomp/tests/xr_math_test.cpp -o /tmp/xr_math_test && /tmp/xr_math_test
#include "platform/xr_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace forza::xr;
constexpr float kDeg = 3.14159265f / 180.0f;
int failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}
bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }
bool SameRotation(const float a[3][3], const float b[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (!Near(a[i][j], b[i][j])) return false;
  return true;
}
// A head-space point through the transform into camera space: x_cam = x_head * r + t.
void HeadToCamera(const forza::vr::HeadTransform& h, const float p[3], float out[3]) {
  for (int j = 0; j < 3; ++j) out[j] = p[0] * h.r[0][j] + p[1] * h.r[1][j] + p[2] * h.r[2][j] + h.t[j];
}

void TestAxesMatchTheTestPose() {
  // OpenXR rotations about +y, +x and +z are the game's yaw (left), pitch (up) and roll (left).
  const float angles[] = {-120, -35, 0, 20, 75, 160};
  for (float a : angles) {
    float expected[3][3];
    forza::vr::HeadRotation({a, 0, 0}, expected);
    Check(SameRotation(ToGameHead({AxisY(a * kDeg), {}}, 0).r, expected), "yaw = rotation about +y");
    forza::vr::HeadRotation({0, a, 0}, expected);
    Check(SameRotation(ToGameHead({AxisX(a * kDeg), {}}, 0).r, expected), "pitch = rotation about +x");
    forza::vr::HeadRotation({0, 0, a}, expected);
    Check(SameRotation(ToGameHead({AxisZ(a * kDeg), {}}, 0).r, expected), "roll = rotation about +z");
  }
  // Combined: yaw, then pitch about the turned head, then roll (q = qy * qx * qz).
  const Quat q = Mul(Mul(AxisY(40 * kDeg), AxisX(-15 * kDeg)), AxisZ(10 * kDeg));
  float expected[3][3];
  forza::vr::HeadRotation({40, -15, 10}, expected);
  Check(SameRotation(ToGameHead({q, {}}, 0).r, expected), "yaw * pitch * roll");
}

void TestLookingLeftSeesLeft() {
  // Turned 90 degrees left, the head's forward (+z in the game) is the camera's left (-x).
  const auto h = ToGameHead({AxisY(90 * kDeg), {}}, 0);
  const float forward[3] = {0, 0, 1};
  float out[3];
  HeadToCamera(h, forward, out);
  Check(Near(out[0], -1) && Near(out[1], 0) && Near(out[2], 0), "left turn looks to -x");
}

void TestPositionAndSeat() {
  // 10 cm right, 5 cm up, 20 cm back (OpenXR +z) -> game x +0.1, y +0.05 + seat, z -0.2.
  const auto h = ToGameHead({{}, {0.1f, 0.05f, 0.2f}}, 0.03f);
  Check(Near(h.t[0], 0.1f) && Near(h.t[1], 0.08f) && Near(h.t[2], -0.2f), "offset and seat height");
}

void TestRecentre() {
  // A head that sits 0.3 m right and looks 30 degrees left, tilted down: after recentring there,
  // the same pose is straight ahead at the origin, except for the pitch.
  const Pose head{Mul(AxisY(30 * kDeg), AxisX(-20 * kDeg)), {0.3f, 1.2f, -0.1f}};
  const Pose origin = RecenterOrigin(head);
  Check(Near(origin.p.x, 0.3f) && Near(origin.p.y, 1.2f) && Near(origin.p.z, -0.1f), "origin position");
  const Pose rel = Relative(origin, head);
  Check(Near(rel.p.x, 0) && Near(rel.p.y, 0) && Near(rel.p.z, 0), "relative position zero");
  float expected[3][3];
  forza::vr::HeadRotation({0, -20, 0}, expected);
  Check(SameRotation(ToGameHead(rel, 0).r, expected), "only the pitch is left");
  // Moving 0.1 m along the recentred forward (head yaw 30 deg left) is +z in the game.
  const Vec3 fwd = Rotate(AxisY(30 * kDeg), {0, 0, -0.1f});
  const Pose moved{head.q, {head.p.x + fwd.x, head.p.y + fwd.y, head.p.z + fwd.z}};
  const auto h = ToGameHead(Relative(origin, moved), 0);
  Check(Near(h.t[0], 0) && Near(h.t[1], 0) && Near(h.t[2], 0.1f), "forward after recentring");
  // Compose undoes Relative.
  const Pose back = Compose(origin, rel);
  Check(Near(back.p.x, head.p.x) && Near(back.p.z, head.p.z) && Near(std::fabs(back.q.w), std::fabs(head.q.w)),
        "compose(origin, relative) = head");
}

void TestYawOnlyKeepsHeading() {
  const Quat yaw = YawOnly(Mul(AxisY(-70 * kDeg), AxisZ(25 * kDeg)));
  const Vec3 f = Rotate(yaw, {0, 0, -1});
  Check(Near(f.y, 0) && Near(std::atan2(-f.x, -f.z), -70 * kDeg), "heading without the roll");
}

void TestCoverFov() {
  // Quest 3-like eyes: about 52/43 degrees out/in, 48 up, 52 down.
  const Fov eyes[2] = {{-52 * kDeg, 43 * kDeg, 48 * kDeg, -52 * kDeg},
                       {-43 * kDeg, 52 * kDeg, 48 * kDeg, -52 * kDeg}};
  const float v = CoverVerticalFov(eyes, 16.0f / 9.0f, 5 * kDeg, 2.2f);
  Check(Near(v, 2 * 57 * kDeg, 1e-3f), "vertical: the larger half-angle plus the margin");
  const Fov drawn = SymmetricFov(v, 16.0f / 9.0f);
  Check(drawn.right >= 57 * kDeg && Near(drawn.up, 57 * kDeg, 1e-3f) && Near(drawn.left, -drawn.right),
        "the drawn 16:9 image covers both eyes and the margin");
  // A tall image: the horizontal need decides.
  const float tall = CoverVerticalFov(eyes, 0.5f, 0, 3.0f);
  Check(Near(std::atan(std::tan(tall / 2) * 0.5f), 52 * kDeg, 1e-3f), "horizontal decides at 1:2");
  // The cap.
  Check(Near(CoverVerticalFov(eyes, 16.0f / 9.0f, 30 * kDeg, 1.5f), 1.5f), "capped");
}

void TestSymmetricFov() {
  const Fov f = SymmetricFov(62 * kDeg, 16.0f / 9.0f);
  Check(Near(f.up, 31 * kDeg) && Near(f.down, -31 * kDeg), "vertical halves");
  Check(Near(std::tan(f.right), std::tan(31 * kDeg) * 16.0f / 9.0f), "horizontal from the aspect");
}

void TestLatency() {
  LatencyEstimate l(40'000'000, 5'000'000, 50'000'000);
  for (int i = 0; i < 100; ++i) l.Add(20'000'000);
  Check(std::llabs(l.Value() - 20'000'000) < 100'000, "converges");
  for (int i = 0; i < 100; ++i) l.Add(300'000'000);
  Check(l.Value() == 50'000'000, "capped at 50 ms");
  l.Add(-5);
  Check(l.Value() == 50'000'000, "ignores negative samples");
}
}  // namespace

int main() {
  TestAxesMatchTheTestPose();
  TestLookingLeftSeesLeft();
  TestPositionAndSeat();
  TestRecentre();
  TestYawOnlyKeepsHeading();
  TestCoverFov();
  TestSymmetricFov();
  TestLatency();
  if (failures) return 1;
  std::printf("xr_math_test: ok\n");
  return 0;
}
