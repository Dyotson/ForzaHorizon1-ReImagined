// The game's rumble on a force feedback wheel's motor (SDK patch 0129, src/input/wheel/wheel_rumble.h,
// #273): classic (one 25 Hz sine, as before) and fine (a light 40 Hz sine for the road motor and a
// ~71 Hz one for the engine motor). Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; g++ -std=c++17 -Wall -Wextra -Werror -I $SDK/include
//       -I $SDK/src/input/wheel forza-recomp/tests/wheel_rumble_test.cpp -o wheel-rumble-test
//   ./wheel-rumble-test
#include <wheel_rumble.h>

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

bool Near(float a, float b) {
  return std::fabs(a - b) < 1e-4f;
}
}  // namespace

int main() {
  // --- rumble --------------------------------------------------------------------------------
  Check(ParseRumbleStyle("classic") == RumbleStyle::kClassic, "classic");
  Check(ParseRumbleStyle("fine") == RumbleStyle::kFine && ParseRumbleStyle("") == RumbleStyle::kFine,
        "fine is the default");
  FfStrengths full;
  // Classic: the stronger motor at 60 percent, 25 Hz, as before #273.
  RumbleWave c = ClassicRumble(65535, 32768, full, 1.0f);
  Check(c.period_ms == 40 && Near(c.level, 0.6f), "classic full road motor");
  FfStrengths st;
  st.overall = 0.6f;
  st.engine = 0.5f;
  st.road = 1.0f;
  c = ClassicRumble(0, 65535, st, 1.0f);
  Check(Near(c.level, 0.6f * 0.5f * 0.6f), "classic engine motor by its strength");
  // Fine: each motor its own sine, at most a quarter of the force, quicker than classic.
  FineRumble f = FineRumbleOf(65535, 0, full, 1.0f);
  Check(f.road.period_ms == 25 && Near(f.road.level, 0.25f) && f.engine.level == 0.0f,
        "fine road motor: 40 Hz, a quarter");
  f = FineRumbleOf(0, 65535, full, 1.0f);
  Check(f.engine.period_ms == 14 && Near(f.engine.level, 0.25f) && f.road.level == 0.0f,
        "fine engine motor: ~71 Hz");
  Check(1000 / f.engine.period_ms >= 40 && 1000 / f.engine.period_ms <= 80 &&
            1000 / f.road.period_ms >= 40 && 1000 / f.road.period_ms <= 80,
        "fine frequencies in 40-80 Hz");
  f = FineRumbleOf(32768, 65535, st, 0.5f);
  Check(Near(f.road.level, 32768 / 65535.0f * 1.0f * 0.6f * 0.25f * 0.5f) &&
            Near(f.engine.level, 1.0f * 0.5f * 0.6f * 0.25f * 0.5f),
        "fine levels scale by strengths and the fade");
  // Both motors full in fine stay below the classic level of one.
  f = FineRumbleOf(65535, 65535, full, 1.0f);
  Check(f.road.level + f.engine.level < ClassicRumble(65535, 65535, full, 1.0f).level,
        "fine is lighter than classic");
  Check(RumbleMagnitude(1.5f) == 32767 && RumbleMagnitude(-1.0f) == 0 &&
            RumbleMagnitude(0.25f) == 8191,
        "SDL magnitudes");
  std::puts("wheel_rumble_test: ok");
  return 0;
}
