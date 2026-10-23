// The minimum steering force (SDK patch 0131, src/input/wheel/wheel_min_force.h, #303): the game's
// weak steering forces are scaled up, strong ones are left alone, and the curve is continuous.
// Build against the patched SDK source (tools/build_rexglue_sdk.sh --source-only), from the
// repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; g++ -std=c++17 -Wall -Wextra -Werror -I $SDK/include
//       -I $SDK/src/input/wheel forza-recomp/tests/wheel_min_force_test.cpp -o wheel-min-force-test
//   ./wheel-min-force-test
#include <wheel_min_force.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace rex::input::wheel;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

bool Near(float a, float b, float eps = 1e-4f) {
  return std::fabs(a - b) < eps;
}

FfEffect Constant(int magnitude, uint32_t gain = 255) {
  FfEffect e;
  e.block = 5;
  e.type = kFfConstant;
  e.gain = gain;
  e.magnitude = magnitude;
  return e;
}
}  // namespace

int main() {
  // Off: every force as the game sends it.
  for (float x : {0.0f, 0.01f, 0.2f, 0.5f, 1.0f}) {
    Check(MinForce(x, 0.0f) == x, "0 is off");
  }
  // Strong forces untouched; the weakest at most 4 times; 0 stays 0 (no step at the centre).
  const float m = 0.3f;
  for (float x : {0.3f, 0.31f, 0.5f, 0.9f, 1.0f}) {
    Check(MinForce(x, m) == x, "at and above the minimum: unchanged");
  }
  Check(MinForce(0.0f, m) == 0.0f, "no force stays no force");
  Check(Near(MinForce(0.075f, m), 0.075f * (1.0f + 3.0f * 0.5625f)), "a quarter of the minimum");
  Check(Near(MinForce(0.15f, m), 0.2625f), "half the minimum plays at 7/8 of it");
  Check(MinForce(0.001f, m) / 0.001f > 3.9f && MinForce(0.001f, m) / 0.001f <= 4.0f,
        "near 0: up to 4 times");
  // Scaled up, never clipped to a floor: below the minimum it rises with the game's force, never
  // weaker than it; continuous (and smooth) at the minimum; never past the full force.
  float prev = 0.0f;
  for (int i = 1; i <= 1000; i++) {
    const float x = i / 1000.0f;
    for (float mm : {0.05f, 0.3f, 0.6f, 1.0f}) {
      const float y = MinForce(x, mm);
      Check(y >= x && y <= 1.0f, "never weaker than the game's force, never past full");
      if (mm == 0.3f) {
        Check(y > prev, "always rising");
        prev = y;
      }
    }
  }
  Check(Near(MinForce(m - 1e-5f, m), m, 1e-4f), "continuous at the minimum");
  Check(Near((MinForce(m, m) - MinForce(m - 1e-3f, m)) / 1e-3f, 1.0f, 0.01f),
        "slope 1 where it meets the line");
  Check(MinForce(2.0f, m) == 1.0f && MinForce(-1.0f, m) == 0.0f, "clamped to 0..1");

  // The level as played: the minimum applies to the game's force (magnitude, effect and device
  // gain), the player's strength and the fade scale it afterwards, and the sign stays.
  const FfEffect weak = Constant(-13);  // about 10 percent, pulling left
  const float strength = 0.6f;
  const float level = FfConstantLevel(weak, FfScale(weak, 255, strength));
  const float boosted = FfMinForceLevel(weak, 255, level, m);
  const float x = 13.0f / 127.0f;
  Check(boosted < 0.0f && Near(boosted, -MinForce(x, m) * strength), "weak force: scaled up, signed");
  const FfEffect strong = Constant(100);
  const float strong_level = FfConstantLevel(strong, FfScale(strong, 255, strength));
  Check(FfMinForceLevel(strong, 255, strong_level, m) == strong_level, "strong force: as it was");
  Check(FfMinForceLevel(weak, 255, level, 0.0f) == level, "off: as it was");
  // The fade still takes it to 0, linearly.
  const float faded = FfConstantLevel(weak, FfScale(weak, 255, strength * 0.5f));
  Check(Near(FfMinForceLevel(weak, 255, faded, m), boosted * 0.5f), "the fade scales the result");
  Check(FfMinForceLevel(weak, 255, 0.0f, m) == 0.0f, "faded out: 0");
  // The device gain and the effect's gain are part of the game's force.
  const FfEffect half_gain = Constant(127, 128);
  const float hg = FfConstantLevel(half_gain, FfScale(half_gain, 128, 1.0f));
  const float game = 128.0f / 255.0f * 128.0f / 255.0f;
  Check(Near(FfMinForceLevel(half_gain, 128, hg, 0.5f), MinForce(game, 0.5f)), "gains count");
  Check(Constant(0).magnitude == 0 && FfMinForceLevel(Constant(0), 255, 0.0f, m) == 0.0f,
        "no force: nothing");
  std::puts("wheel min force: ok");
  return 0;
}
