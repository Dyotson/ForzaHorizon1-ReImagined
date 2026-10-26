// forza - the VR cockpit's cabin drawn from where the eye is (#333, docs/vr.md "Version 8"): the math
// of src/hooks/vr_cockpit_space.cpp, without the game.
//
// The game draws the player's car interior in a pass of its own (base sub_82DCF1C0, TU4
// sub_82E91040) whose coordinates have their origin at the car's in-car camera point (the car
// render object's +1872, x y z w in signed fixed point with 18 fraction bits). The camera's offset
// from that point is (camera - point) in the same fixed point, and when its squared length is under
// 0.01 (10 cm) the game uses zero instead: the cabin is drawn as if the eye were exactly at the
// point. That keeps the cabin still under the camera's shake on a monitor; in a headset it makes the
// cabin follow every head movement within 10 cm and, as the light is computed from where the eye
// really is, slides the shadows over it.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace forza::vr {

// AltiVec vcfpsxws with 18 fraction bits: x * 2^18 rounded toward zero, saturated (NaN: 0).
inline int32_t ToFixed18(float x) {
  if (std::isnan(x)) return 0;
  const double v = std::trunc(double(x) * 262144.0);
  if (v >= double(std::numeric_limits<int32_t>::max())) return std::numeric_limits<int32_t>::max();
  if (v <= double(std::numeric_limits<int32_t>::min())) return std::numeric_limits<int32_t>::min();
  return int32_t(v);
}

// vsubsws: a - b saturated.
inline int32_t SubSat(int32_t a, int32_t b) {
  const int64_t d = int64_t(a) - int64_t(b);
  if (d > std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
  if (d < std::numeric_limits<int32_t>::min()) return std::numeric_limits<int32_t>::min();
  return int32_t(d);
}

// The camera's offset from the in-car camera point as the game computes it before its 10 cm test:
// camera (x y z w floats) minus point (fixed point), all four components.
inline void CockpitOffset(const float camera[4], const int32_t point[4], float out[4]) {
  for (int i = 0; i < 4; ++i) out[i] = -float(SubSat(point[i], ToFixed18(camera[i]))) / 262144.0f;
}

// The game's test: offsets shorter than this (squared, metres) are replaced by zero.
constexpr float kCockpitSnapSquared = 0.01f;
inline bool GameSnaps(const float offset[4]) {
  return offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2] < kCockpitSnapSquared;
}

// The view (world to camera) of a rigid camera-to-world matrix, row vectors, rows 0-2 the axes and
// row 3 the position: rotation transposed, translation -position . axis.
inline void RigidInverse(const float m[16], float out[16]) {
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) out[4 * i + j] = m[4 * j + i];
    out[4 * i + 3] = 0;
  }
  for (int j = 0; j < 3; ++j)
    out[12 + j] = -(m[12] * m[4 * j] + m[13] * m[4 * j + 1] + m[14] * m[4 * j + 2]);
  out[15] = 1;
}

}  // namespace forza::vr
