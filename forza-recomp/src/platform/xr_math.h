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

// The cockpit view the game draws: a symmetric frustum `vertical` radians tall whose width is
// set by `aspect` = tan(horizontal half-angle) / tan(vertical half-angle), around an axis turned
// `yaw` (positive left) and `pitch` (positive up) from the head's forward. The game's picture
// keeps its pixels (16:9 at the guest's resolution): with an aspect other than the picture's
// the pixels are not square, and none fall outside the frustum.
struct CockpitFrustum {
  float vertical = 0;
  float aspect = 0;
  float yaw = 0, pitch = 0;
  float HalfHorizontal() const { return std::atan(std::tan(vertical / 2) * aspect); }
  // The XrFovf of the picture, relative to the turned axis.
  Fov ImageFov() const {
    const float h = HalfHorizontal();
    return {-h, h, vertical / 2, -vertical / 2};
  }
  // The turn from the head to the drawn axis, as an OpenXR rotation (yaw about +y, then pitch
  // about the turned +x).
  Quat Offset() const { return Normalize(Mul(AxisY(yaw), AxisX(pitch))); }
};

// The yaw (positive left) and pitch (positive up) of where an orientation looks.
inline void YawPitch(const Quat& q, float& yaw, float& pitch) {
  const Vec3 f = Rotate(q, {0, 0, -1});
  yaw = std::atan2(-f.x, -f.z);
  pitch = std::atan2(f.y, std::sqrt(f.x * f.x + f.z * f.z));
}

// The smallest cockpit frustum that covers both eyes' fields of view plus `margin` (radians on
// each side). `eye_rotation` is each eye's orientation relative to the head (identity for the
// parallel displays of a Quest 3; canted displays shift their edges). The union of the eyes is
// usually taller below the horizon than above (Quest 3: 44 up, 55 down), so the drawn axis is
// pitched to its middle instead of drawing a symmetric frustum as tall as the larger side.
// Half-angles are capped at max_half (tan() stays finite) and the height at max_vertical.
inline CockpitFrustum CoverEyes(const Fov eyes[2], const Quat eye_rotation[2], float margin,
                                float max_half, float max_vertical) {
  // The axis: the middle of the eyes' edges (an eye turned left, positive yaw, moves its edges
  // toward negative angles).
  float left = 0, right = 0, up = 0, down = 0;
  for (int i = 0; i < 2; ++i) {
    float yaw, pitch;
    YawPitch(eye_rotation[i], yaw, pitch);
    const float l = eyes[i].left - yaw, r = eyes[i].right - yaw;
    const float u = eyes[i].up + pitch, d = eyes[i].down + pitch;
    left = i ? std::min(left, l) : l;
    right = i ? std::max(right, r) : r;
    up = i ? std::max(up, u) : u;
    down = i ? std::min(down, d) : d;
  }
  CockpitFrustum f;
  f.yaw = -(left + right) / 2;
  f.pitch = (up + down) / 2;
  // The extent: each eye's frustum corners seen from the turned axis (a turned frustum's widest
  // and tallest points are its corners; pitching the axis widens the corners a little).
  const Quat to_axis = Conj(f.Offset());
  float tan_h = 0, tan_v = 0;
  for (int i = 0; i < 2; ++i) {
    const Quat eye = Mul(to_axis, eye_rotation[i]);
    for (float h : {eyes[i].left, eyes[i].right}) {
      for (float v : {eyes[i].up, eyes[i].down}) {
        const Vec3 d = Rotate(eye, {std::tan(h), std::tan(v), -1});
        const float depth = std::max(-d.z, 1e-3f);
        tan_h = std::max(tan_h, std::fabs(d.x) / depth);
        tan_v = std::max(tan_v, std::fabs(d.y) / depth);
      }
    }
  }
  const float half_h = std::clamp(std::atan(tan_h) + margin, 0.05f, max_half);
  const float half_v = std::clamp(std::atan(tan_v) + margin, 0.05f, std::min(max_half, max_vertical / 2));
  f.vertical = 2 * half_v;
  f.aspect = std::tan(half_h) / std::tan(half_v);
  return f;
}

// The frustum version 1 drew, for comparison (docs/vr.md): a symmetric vertical field of view
// covering the larger half-angle of either axis at the picture's own aspect.
inline CockpitFrustum SymmetricCover(const Fov eyes[2], float picture_aspect, float margin,
                                     float max_vertical) {
  float half_h = 0, half_v = 0;
  for (int i = 0; i < 2; ++i) {
    half_h = std::max({half_h, -eyes[i].left, eyes[i].right});
    half_v = std::max({half_v, eyes[i].up, -eyes[i].down});
  }
  constexpr float kLimit = 1.5f;
  half_h = std::min(half_h + margin, kLimit);
  half_v = std::min(half_v + margin, kLimit);
  const float from_h = std::atan(std::tan(half_h) / std::max(picture_aspect, 0.1f));
  CockpitFrustum f;
  f.vertical = std::min(2 * std::max(half_v, from_h), max_vertical);
  f.aspect = picture_aspect;
  return f;
}

// Pixels per degree at the centre of a picture `pixels` wide (or tall) drawn over a symmetric
// rectilinear half-angle `half`.
inline float CentrePixelsPerDegree(float pixels, float half) {
  return pixels / 2 / std::tan(half) * 3.14159265f / 180;
}

// A recentred head pose held at most `back` metres behind the recentred position (+z is backwards
// in the recentred frame). Turning round moves the eyes back with the neck and the shoulders, and
// the driver's seat is only about 20-25 cm behind the game's eye point: further back, the near
// plane cuts through the seat and the view lands inside it, which looks like another camera.
inline Pose LimitBack(Pose rel, float back) {
  rel.p.z = std::min(rel.p.z, back);
  return rel;
}

// The head transform for a view turned by the frustum's offset (yaw, then pitch, in head space).
inline Pose TurnView(const Pose& head, const CockpitFrustum& f) {
  return {Normalize(Mul(head.q, f.Offset())), head.p};
}

// Yaw (positive left), pitch (positive up) and roll (positive tilts left) in radians of an
// orientation, applied in that order (as vr::HeadPose): for logs.
inline void YawPitchRoll(const Quat& q, float& yaw, float& pitch, float& roll) {
  YawPitch(q, yaw, pitch);
  // Roll: the head's up axis against the up the yaw and pitch alone would give.
  const Quat no_roll = Mul(AxisY(yaw), AxisX(pitch));
  const Vec3 up = Rotate(Mul(Conj(no_roll), q), {0, 1, 0});
  roll = std::atan2(-up.x, up.y);
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
