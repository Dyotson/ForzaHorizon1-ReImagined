// forza - pose and field-of-view maths for the OpenXR path (docs/vr.md, #333), without OpenXR
// headers so tests/xr_math_test.cpp can check it on its own.
//
// OpenXR spaces are right-handed: x right, y up, z backwards (the view looks along -z). The
// game's camera space is left-handed: x right, y up, z forwards (vr_head_pose_math.h). A head
// pose in the recentred LOCAL space becomes a HeadTransform by flipping z on both sides.
#pragma once

#include "vr_head_pose_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace forza::xr {

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};
struct Vec3 {
  float x = 0, y = 0, z = 0;
};
// An OpenXR pose: orientation and position.
struct Pose {
  Quat q;
  Vec3 p;
};
// XrFovf: radians, left and down negative.
struct Fov {
  float left = 0, right = 0, up = 0, down = 0;
};

inline Quat Mul(const Quat& a, const Quat& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat Conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat Normalize(const Quat& q) {
  const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n < 1e-6f) return {};
  return {q.x / n, q.y / n, q.z / n, q.w / n};
}
inline Vec3 Rotate(const Quat& q, const Vec3& v) {
  const Quat r = Mul(Mul(q, Quat{v.x, v.y, v.z, 0}), Conj(q));
  return {r.x, r.y, r.z};
}
// Rotation about +y by `radians` (positive turns left).
inline Quat AxisY(float radians) { return {0, std::sin(radians / 2), 0, std::cos(radians / 2)}; }
inline Quat AxisX(float radians) { return {std::sin(radians / 2), 0, 0, std::cos(radians / 2)}; }
inline Quat AxisZ(float radians) { return {0, 0, std::sin(radians / 2), std::cos(radians / 2)}; }

// The heading of an orientation (the rotation about +y that turns -z to where it looks,
// projected on the floor), so a recentre keeps the horizon level.
inline Quat YawOnly(const Quat& q) {
  const Vec3 forward = Rotate(q, {0, 0, -1});
  if (std::fabs(forward.x) < 1e-6f && std::fabs(forward.z) < 1e-6f) return {};  // straight up/down
  return AxisY(std::atan2(-forward.x, -forward.z));
}

// `head` relative to `origin` (both in the same space): origin^-1 * head.
inline Pose Relative(const Pose& origin, const Pose& head) {
  const Quat inv = Conj(origin.q);
  const Vec3 d{head.p.x - origin.p.x, head.p.y - origin.p.y, head.p.z - origin.p.z};
  return {Normalize(Mul(inv, head.q)), Rotate(inv, d)};
}
// origin * local: a pose given relative to `origin`, in origin's space.
inline Pose Compose(const Pose& origin, const Pose& local) {
  const Vec3 d = Rotate(origin.q, local.p);
  return {Normalize(Mul(origin.q, local.q)), {origin.p.x + d.x, origin.p.y + d.y, origin.p.z + d.z}};
}

// The recentre origin for a head pose: where the head is, turned like it on the floor.
inline Pose RecenterOrigin(const Pose& head) { return {YawOnly(head.q), head.p}; }

// The game's head transform for a head pose relative to the recentre origin, with the seat's
// height offset (metres, positive raises the eye).
inline vr::HeadTransform ToGameHead(const Pose& rel, float seat_height) {
  const Quat q = Normalize(rel.q);
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  // Column-vector rotation in OpenXR space: v_space = m * v_head.
  const float m[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                         {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                         {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
  // Flip z on both sides (S m S, S = diag(1, 1, -1)) and transpose for the game's row vectors.
  const float s[3] = {1, 1, -1};
  vr::HeadTransform h;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) h.r[i][j] = s[i] * s[j] * m[j][i];
  h.t[0] = rel.p.x;
  h.t[1] = rel.p.y + seat_height;
  h.t[2] = -rel.p.z;
  return h;
}

// The vertical field of view (radians, symmetric) the game should draw at `aspect` (width /
// height) so its image covers both eyes' fields of view plus `margin` (radians on each side),
// capped at max_vertical.
inline float CoverVerticalFov(const Fov eyes[2], float aspect, float margin, float max_vertical) {
  float half_h = 0, half_v = 0;
  for (int i = 0; i < 2; ++i) {
    half_h = std::max({half_h, -eyes[i].left, eyes[i].right});
    half_v = std::max({half_v, eyes[i].up, -eyes[i].down});
  }
  half_h += margin;
  half_v += margin;
  constexpr float kLimit = 1.5f;  // below 90 degrees per side: tan() stays finite
  half_h = std::min(half_h, kLimit);
  half_v = std::min(half_v, kLimit);
  // The horizontal half-angle follows from the vertical one and the aspect ratio.
  const float from_h = std::atan(std::tan(half_h) / std::max(aspect, 0.1f));
  return std::min(2 * std::max(half_v, from_h), max_vertical);
}

// The XrFovf of an image the game drew with a symmetric vertical field of view at `aspect`.
inline Fov SymmetricFov(float vertical, float aspect) {
  const float up = vertical / 2;
  const float right = std::atan(std::tan(up) * aspect);
  return {-right, right, up, -up};
}

// A latency estimate (ns): an exponential moving average of measured samples, clamped.
class LatencyEstimate {
 public:
  LatencyEstimate(int64_t initial, int64_t lo, int64_t hi) : value_(initial), lo_(lo), hi_(hi) {}
  void Add(int64_t sample) {
    if (sample <= 0) return;
    value_ += (std::clamp(sample, lo_, 4 * hi_) - value_) / 8;
  }
  int64_t Value() const { return std::clamp(value_, lo_, hi_); }
  int64_t Raw() const { return value_; }

 private:
  int64_t value_, lo_, hi_;
};

}  // namespace forza::xr
