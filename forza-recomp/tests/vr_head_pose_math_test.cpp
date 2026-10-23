// Standalone check of the VR prototype's head pose math (src/platform/vr_head_pose_math.h).
// c++ -std=c++17 forza-recomp/tests/vr_head_pose_math_test.cpp -o /tmp/vr_test && /tmp/vr_test
#include "../src/platform/vr_head_pose_math.h"
#include <cassert>
#include <cmath>
#include <cstdio>

namespace {
// A world point through a row-vector view matrix.
void Transform(const float m[16], const float p[3], float out[3]) {
  for (int j = 0; j < 3; ++j)
    out[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
}
bool Near(const float a[3], float x, float y, float z) {
  return std::fabs(a[0] - x) < 1e-4f && std::fabs(a[1] - y) < 1e-4f && std::fabs(a[2] - z) < 1e-4f;
}
// A camera at (10, 2, 5) looking along world +x (right = world -z, up = +y), as the game
// stores it: columns of the upper 3x3 are the camera axes, row 3 the translation.
void Camera(float m[16]) {
  const float right[3] = {0, 0, -1}, up[3] = {0, 1, 0}, fwd[3] = {1, 0, 0}, pos[3] = {10, 2, 5};
  const float* axes[3] = {right, up, fwd};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m[i * 4 + j] = axes[j][i];
    m[i * 4 + 3] = 0;
  }
  for (int j = 0; j < 3; ++j)
    m[12 + j] = -(pos[0] * axes[j][0] + pos[1] * axes[j][1] + pos[2] * axes[j][2]);
  m[15] = 1;
}
}  // namespace

int main() {
  using forza::vr::ApplyHeadPose;
  using forza::vr::HeadPose;
  float v[16], c[3];
  const float ahead[3] = {11, 2, 5}, left[3] = {10, 2, 6}, above[3] = {10, 3, 5};
  Camera(v);
  Transform(v, ahead, c);
  assert(Near(c, 0, 0, 1));
  // Identity pose: unchanged.
  ApplyHeadPose(v, HeadPose{});
  Transform(v, ahead, c);
  assert(Near(c, 0, 0, 1));
  // Yaw 90 degrees left: the point on the camera's left is now straight ahead.
  Camera(v);
  ApplyHeadPose(v, HeadPose{90, 0, 0, 0, 0, 0});
  Transform(v, left, c);
  assert(Near(c, 0, 0, 1));
  Transform(v, ahead, c);
  assert(Near(c, 1, 0, 0));  // what was ahead is now on the right
  // Pitch 90 degrees up: the point above is ahead.
  Camera(v);
  ApplyHeadPose(v, HeadPose{0, 90, 0, 0, 0, 0});
  Transform(v, above, c);
  assert(Near(c, 0, 0, 1));
  // Roll 90 degrees left (counterclockwise): the point above appears on the right.
  Camera(v);
  ApplyHeadPose(v, HeadPose{0, 0, 90, 0, 0, 0});
  Transform(v, above, c);
  assert(Near(c, 1, 0, 0));
  // Moving the head 0.5 m right puts the point ahead 0.5 m to the left.
  Camera(v);
  ApplyHeadPose(v, HeadPose{0, 0, 0, 0.5f, 0, 0});
  Transform(v, ahead, c);
  assert(Near(c, -0.5f, 0, 1));
  // Yaw then pitch: turning left 90 and looking up 45 sees a point up and to the left.
  Camera(v);
  ApplyHeadPose(v, HeadPose{90, 45, 0, 0, 0, 0});
  const float up_left[3] = {10, 3, 6};
  Transform(v, up_left, c);
  assert(Near(c, 0, 0, std::sqrt(2.0f)));
  std::puts("vr_head_pose_math_test: ok");
  return 0;
}
