#include "../src/platform/garage_camera_math.h"
#include <cassert>
#include <cmath>
#include <initializer_list>
#include <limits>

int main() {
  using forza::garage_camera::CorrectOrbit;
  using forza::garage_camera::OrbitAngle;
  constexpr float step = 0.001745329238474369f;
  constexpr double turn = 6.283185307179586;
  for (int fps : {30, 60, 120, 144, 240}) {
    float angle = 0;
    for (int n = 0; n < fps * 10; ++n)
      angle = CorrectOrbit(float(std::fmod(angle + step, turn)), 1.0 / fps);
    assert(std::abs(angle - step * 30 * 10) < 0.0001);
  }
  // A stalled renderer must use elapsed time rather than the requested cap.
  assert(std::abs(CorrectOrbit(step, 0.1) - step * 3) < 1e-7);
  assert(std::abs(CorrectOrbit(step, 0)) < 1e-7);
  assert(CorrectOrbit(1, -1) == 1);
  assert(CorrectOrbit(1, std::numeric_limits<double>::quiet_NaN()) == 1);
  assert(CorrectOrbit(1, std::numeric_limits<double>::infinity()) == 1);
  auto wrapped = CorrectOrbit(0.0001f, 1.0 / 120);
  assert(wrapped > 6 && wrapped < turn);
  // A timer that was not reset (29 s in the paint shop) is not one update.
  assert(CorrectOrbit(1, 29.4) == 1);
  assert(CorrectOrbit(1, 0.26) == 1);
  assert(std::abs(CorrectOrbit(step, 0.25) - step * 7.5) < 1e-7);
  // Only the automatic orbit (state 2) is corrected. A move to a preset (state 1, and 0
  // on its last frame) keeps its angle at any frame rate and timer value (#243).
  for (unsigned state : {0u, 1u, 3u})
    for (double dt : {1.0 / 240, 1.0 / 60, 1.0 / 30, 0.2, 29.4})
      assert(OrbitAngle(state, 4.6835f, dt) == 4.6835f);
  assert(OrbitAngle(2, step, 1.0 / 60) == CorrectOrbit(step, 1.0 / 60));
  // Ten preset moves at 60 fps leave the camera where the guest put it.
  float preset = 4.6835f;
  for (int n = 0; n < 10; ++n) preset = OrbitAngle(n % 2, preset, 1.0 / 60);
  assert(preset == 4.6835f);
}
