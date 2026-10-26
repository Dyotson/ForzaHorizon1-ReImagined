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

// The Quest 3's eyes as WiVRn reported them (left, right, up, down in degrees).
const Fov kQuest3[2] = {{-54 * kDeg, 40 * kDeg, 44 * kDeg, -55 * kDeg},
                        {-40 * kDeg, 54 * kDeg, 44 * kDeg, -55 * kDeg}};
const Quat kParallel[2] = {{}, {}};

// Every corner of both eyes' frusta lies inside the drawn frustum, with `margin` to spare.
bool Covers(const CockpitFrustum& f, const Fov eyes[2], const Quat rot[2], float margin) {
  const Quat to_axis = Conj(f.Offset());
  const float th = std::tan(f.HalfHorizontal() - margin + 1e-4f), tv = std::tan(f.vertical / 2 - margin + 1e-4f);
  for (int i = 0; i < 2; ++i)
    for (float h : {eyes[i].left, eyes[i].right})
      for (float v : {eyes[i].up, eyes[i].down}) {
        const Vec3 d = Rotate(Mul(to_axis, rot[i]), {std::tan(h), std::tan(v), -1});
        if (-d.z <= 0 || std::fabs(d.x) / -d.z > th || std::fabs(d.y) / -d.z > tv) return false;
      }
  return true;
}

void TestCoverQuest3() {
  const CockpitFrustum f = CoverEyes(kQuest3, kParallel, 5 * kDeg, 1.5f, 2.2f);
  Check(Near(f.yaw, 0) && Near(f.pitch, -5.5f * kDeg), "axis pitched to the middle of 44 up and 55 down");
  Check(Covers(f, kQuest3, kParallel, 5 * kDeg), "both eyes and the margin inside");
  Check(Near(f.vertical, 109 * kDeg, 0.2f * kDeg), "109 degrees tall (version 1: 120)");
  Check(Near(f.HalfHorizontal(), 61.7f * kDeg, 0.2f * kDeg), "123 degrees wide (version 1: 144)");
  // Centre pixel density at render scale 2 (2560x1440): version 1 drew 120 degrees tall at 16:9.
  const CockpitFrustum v1 = SymmetricCover(kQuest3, 16.0f / 9.0f, 5 * kDeg, 2.2f);
  Check(Near(v1.vertical, 120 * kDeg, 1e-3f) && Near(v1.aspect, 16.0f / 9.0f), "version 1: 120 degrees at 16:9");
  const float v1_h = CentrePixelsPerDegree(2560, v1.HalfHorizontal());
  const float v1_v = CentrePixelsPerDegree(1440, v1.vertical / 2);
  const float h = CentrePixelsPerDegree(2560, f.HalfHorizontal());
  const float v = CentrePixelsPerDegree(1440, f.vertical / 2);
  Check(Near(v1_h, 7.26f, 0.02f) && Near(v1_v, 7.26f, 0.02f), "version 1: 7.3 pixels per degree");
  Check(Near(h, 12.0f, 0.05f) && Near(v, 8.96f, 0.05f), "now: 12.0 across, 9.0 up");
  // No margin: exactly the eyes' extent.
  const CockpitFrustum tight = CoverEyes(kQuest3, kParallel, 0, 1.5f, 2.2f);
  Check(Covers(tight, kQuest3, kParallel, 0) && tight.vertical < f.vertical, "without a margin");
}

void TestCoverCantedAndCapped() {
  // Displays canted 10 degrees outward (the left eye turned left), and a headset that sees more
  // to the left: the axis follows, the corners stay covered.
  const Fov eyes[2] = {{-50 * kDeg, 45 * kDeg, 45 * kDeg, -45 * kDeg},
                       {-45 * kDeg, 50 * kDeg, 45 * kDeg, -45 * kDeg}};
  const Quat canted[2] = {AxisY(10 * kDeg), AxisY(-10 * kDeg)};
  const CockpitFrustum f = CoverEyes(eyes, canted, 3 * kDeg, 1.5f, 2.2f);
  Check(Near(f.yaw, 0) && Near(f.pitch, 0), "symmetric canted displays: straight axis");
  Check(Covers(f, eyes, canted, 3 * kDeg), "canted eyes covered");
  Check(Near(f.HalfHorizontal(), 63 * kDeg, 1e-3f), "canting widens the union: 60 plus the margin");
  const Fov lopsided[2] = {{-60 * kDeg, 30 * kDeg, 40 * kDeg, -40 * kDeg},
                           {-50 * kDeg, 40 * kDeg, 40 * kDeg, -40 * kDeg}};
  const CockpitFrustum g = CoverEyes(lopsided, kParallel, 2 * kDeg, 1.5f, 2.2f);
  Check(Near(g.yaw, 10 * kDeg), "turned left to the middle of -60 and 40");
  Check(Covers(g, lopsided, kParallel, 2 * kDeg), "lopsided eyes covered");
  // Caps: half-angles below 86 degrees and the height.
  const CockpitFrustum wide = CoverEyes(kQuest3, kParallel, 30 * kDeg, 1.5f, 2.2f);
  Check(wide.HalfHorizontal() <= 1.5f + 1e-4f && wide.vertical <= 2.2f + 1e-4f, "capped");
}

void TestTurnedView() {
  // The drawn view is the head turned by the frustum's offset, the same in OpenXR (the layer's
  // pose) and in the game (the camera): for a head looking 30 degrees left, the axis pitched
  // 5.5 down equals the test pose (30, -5.5, 0).
  CockpitFrustum f;
  f.pitch = -5.5f * kDeg;
  const Pose head{AxisY(30 * kDeg), {0.05f, 0, 0}};
  const Pose turned = TurnView(head, f);
  float expected[3][3];
  forza::vr::HeadRotation({30, -5.5f, 0}, expected);
  Check(SameRotation(ToGameHead(turned, 0).r, expected), "turned view = yaw then the axis pitch");
  Check(Near(turned.p.x, 0.05f), "position unchanged");
  const Fov fov = f.ImageFov();
  Check(Near(fov.left, -fov.right) && Near(fov.up, -fov.down), "the picture is symmetric about the axis");
}

void TestBackLimit() {
  // Turned round with the head 20 cm back (+z): the eye stops 8 cm back; forward, sideways and
  // up are free.
  const Pose back{AxisY(180 * kDeg), {0.05f, 0.03f, 0.20f}};
  const Pose held = LimitBack(back, 0.08f);
  Check(Near(held.p.z, 0.08f) && Near(held.p.x, 0.05f) && Near(held.p.y, 0.03f), "held 8 cm back");
  const auto h = ToGameHead(held, 0);
  Check(Near(h.t[2], -0.08f), "the game's eye 8 cm behind the driver's point");
  const Pose forward{{}, {0, 0, -0.3f}};
  Check(Near(LimitBack(forward, 0.08f).p.z, -0.3f), "leaning forward is not limited");
  // In LOCAL the held pose is the origin composed with it.
  const Pose origin{AxisY(30 * kDeg), {0.1f, 1.2f, 0.4f}};
  const Pose located = Compose(origin, back);
  const Pose shown = Compose(origin, LimitBack(Relative(origin, located), 0.08f));
  const Vec3 d{shown.p.x - located.p.x, shown.p.y - located.p.y, shown.p.z - located.p.z};
  const Vec3 along = Rotate(Conj(origin.q), d);
  Check(Near(along.x, 0) && Near(along.y, 0) && Near(along.z, -0.12f), "moved 12 cm forward along the recentred axis");
}

void TestYawPitchRoll() {
  const float cases[][3] = {{0, 0, 0}, {35, -10, 5}, {-120, 20, -15}, {179, 0, 0}, {-179, 5, 0}};
  for (const auto& c : cases) {
    const Quat q = Mul(Mul(AxisY(c[0] * kDeg), AxisX(c[1] * kDeg)), AxisZ(c[2] * kDeg));
    float y, p, r;
    YawPitchRoll(q, y, p, r);
    Check(Near(y, c[0] * kDeg, 1e-3f) && Near(p, c[1] * kDeg, 1e-3f) && Near(r, c[2] * kDeg, 1e-3f),
          "yaw, pitch and roll back from the quaternion");
  }
  // Looking straight back: 180 degrees either way, the same view.
  const auto back = ToGameHead({AxisY(180 * kDeg), {}}, 0);
  const auto back2 = ToGameHead({AxisY(-180 * kDeg), {}}, 0);
  Check(SameRotation(back.r, back2.r), "180 and -180 are the same view");
  const float forward[3] = {0, 0, 1};
  float out[3];
  HeadToCamera(back, forward, out);
  Check(Near(out[0], 0) && Near(out[2], -1), "looking back sees the car's rear");
  // A quaternion and its negation (both describe one rotation; runtimes return either).
  const Quat q = AxisY(178 * kDeg);
  const Quat neg{-q.x, -q.y, -q.z, -q.w};
  Check(SameRotation(ToGameHead({q, {}}, 0).r, ToGameHead({neg, {}}, 0).r), "q and -q");
}

void TestLatency() {
  LatencyEstimate l(40'000'000, 5'000'000, 100'000'000);
  for (int i = 0; i < 100; ++i) l.Add(20'000'000);
  Check(std::llabs(l.Value() - 20'000'000) < 100'000, "converges");
  // The Quest 3 over WiVRn measured 70-95 ms: predicted as measured (version 1 stopped at 50).
  for (int i = 0; i < 100; ++i) l.Add(82'000'000);
  Check(std::llabs(l.Value() - 82'000'000) < 100'000, "follows a streamed headset's latency");
  for (int i = 0; i < 100; ++i) l.Add(300'000'000);
  Check(l.Value() == 100'000'000, "capped at 100 ms");
  l.Add(-5);
  Check(l.Value() == 100'000'000, "ignores negative samples");
}
}  // namespace

int main() {
  TestAxesMatchTheTestPose();
  TestLookingLeftSeesLeft();
  TestPositionAndSeat();
  TestRecentre();
  TestYawOnlyKeepsHeading();
  TestCoverQuest3();
  TestCoverCantedAndCapped();
  TestTurnedView();
  TestYawPitchRoll();
  TestBackLimit();
  TestLatency();
  if (failures) return 1;
  std::printf("xr_math_test: ok\n");
  return 0;
}
