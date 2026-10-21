// The racing wheel's button map (SDK patch 0101, rex/input/wheel/wheel_buttons.h): with the game's
// wheel layouts the paddles must be the bumpers, which shift (#219). Build against the patched SDK
// source (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   g++ -std=c++17 -Wall -Wextra -Werror -I ~/rexglue-sdk-src-forza/sdk/include
//       forza-recomp/tests/wheel_buttons_test.cpp -o wheel-buttons-test && ./wheel-buttons-test
#include <rex/input/wheel/wheel_buttons.h>

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
}  // namespace

int main() {
  // G29 / G923 (PlayStation): cross, square, circle, triangle, right paddle, left paddle, R2, L2,
  // Share, Options, R3, L3 are SDL buttons 0-11.
  auto g29 = WheelButtons(true, {});
  Check(g29["rb"] == 4 && g29["lb"] == 5, "G29: the paddles are the bumpers (shift up, down)");
  Check(g29["a"] == 0 && g29["b"] == 2 && g29["x"] == 1 && g29["y"] == 3,
        "G29: cross A (clutch), circle B (handbrake), square X (activate), triangle Y (rewind)");
  Check(g29["back"] == 8 && g29["start"] == 9 && g29["rs"] == 10 && g29["ls"] == 11,
        "G29: Share, Options, R3, L3");
  for (const auto& [k, v] : g29) {
    Check(v != 6 && v != 7, "G29: R2 and L2 press nothing");
  }
  Check(g29["up"] == -1 && g29["down"] == -1 && g29["left"] == -1 && g29["right"] == -1,
        "the d-pad comes from the hat");

  // Other wheels (G920, G923 for Xbox, unknown ones): face buttons, then the paddles.
  auto generic = WheelButtons(false, {});
  Check(generic["a"] == 0 && generic["b"] == 1 && generic["x"] == 2 && generic["y"] == 3,
        "generic face buttons");
  Check(generic["rb"] == 4 && generic["lb"] == 5, "generic paddles on the bumpers");

  // wheel_buttons overrides win; -1 unmaps; unknown names are ignored.
  auto m = WheelButtons(true, {{"rb", 6}, {"lb", 7}, {"x", -1}, {"turbo", 3}});
  Check(m["rb"] == 6 && m["lb"] == 7 && m["x"] == -1, "overrides");
  Check(m.count("turbo") == 0 && m.size() == generic.size(), "unknown names ignored");
  std::puts("wheel buttons: all checks passed");
  return 0;
}
