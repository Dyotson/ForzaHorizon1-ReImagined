#pragma once
#include <cmath>
#include <cstdint>

namespace forza::garage_camera {
// CUI4CarTargetCamera::Update's state (camera + 0x150). Only state 2, the automatic orbit,
// adds the fixed step. State 1 moves the camera to a preset (the paint shop's sides) on its
// own clock, and its last frame also ends in the view call the orbit hook sees (#243).
constexpr uint32_t kOrbitState = 2;
// Longest elapsed time taken as one update. The camera's timer is not reset while the
// paint shop's decal camera is up (29 s after 30 s there), and a gap this long is a stall.
constexpr double kMaxOrbitStep = 0.25;

// The guest has already added one fixed step and wrapped the angle.
inline float CorrectOrbit(float angle, double dt) {
  if (!std::isfinite(dt) || dt < 0.0 || dt > kMaxOrbitStep) return angle;
  constexpr double step = 0.001745329238474369;
  constexpr double turn = 6.283185307179586;
  double corrected = std::fmod(angle - step + step * 30.0 * dt, turn);
  if (corrected < 0.0) corrected += turn;
  return float(corrected);
}

// The angle the view is built from: corrected only for the automatic orbit.
inline float OrbitAngle(uint32_t state, float angle, double dt) {
  return state == kOrbitState ? CorrectOrbit(angle, dt) : angle;
}
}  // namespace forza::garage_camera
