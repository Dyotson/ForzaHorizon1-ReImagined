// The racing wheel's button map (SDK patch 0101, rex/input/wheel/wheel_buttons.h): with the game's
// wheel layouts the paddles must be the bumpers, which shift (#219); each model's own layout, and
// the shifter's buttons press no pad button (#256); a d-pad direction set in wheel_buttons replaces
// the hat's (patch 0127, #279). Build against the patched SDK
// source (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   g++ -std=c++17 -Wall -Wextra -Werror -I ~/rexglue-sdk-src-forza/sdk/include
//       forza-recomp/tests/wheel_buttons_test.cpp -o wheel-buttons-test && ./wheel-buttons-test
#include <rex/input/wheel/wheel_buttons.h>

#include <cstdio>
#include <cstdlib>
#include <set>

using namespace rex::input::wheel;

namespace {
std::map<std::string, int> ParseKvLike(const char* key, int value) { return {{key, value}}; }

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
  Check(ButtonLayoutOf(0x046D, 0xC24F) == ButtonLayout::kPlayStation &&
            ButtonLayoutOf(0x046D, 0xC266) == ButtonLayout::kPlayStation &&
            ButtonLayoutOf(0x046D, 0xC267) == ButtonLayout::kPlayStation,
        "G29 and PlayStation G923 by vendor:product");
  Check(ButtonLayoutOf(0x046D, 0xC262) == ButtonLayout::kXbox &&
            ButtonLayoutOf(0x046D, 0xC26E) == ButtonLayout::kXbox &&
            ButtonLayoutOf(0x046D, 0xC26D) == ButtonLayout::kXbox,
        "G920 and Xbox G923 by vendor:product");
  Check(ButtonLayoutOf(0x046D, 0xC29B) == ButtonLayout::kGeneric &&
            ButtonLayoutOf(0x044F, 0xB66E) == ButtonLayout::kGeneric,
        "other wheels: the generic order");

  auto g29 = WheelButtons(ButtonLayout::kPlayStation, {});
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

  // G920 and G923 for Xbox: A B X Y, the paddles, Menu, View, RSB, LSB, the Xbox button (10);
  // 11 is the Driving Force Shifter's reverse, 12-17 its gears.
  auto xbox = WheelButtons(ButtonLayout::kXbox, {});
  Check(xbox["a"] == 0 && xbox["b"] == 1 && xbox["x"] == 2 && xbox["y"] == 3, "Xbox: face buttons");
  Check(xbox["rb"] == 4 && xbox["lb"] == 5, "Xbox: the paddles are the bumpers");
  Check(xbox["start"] == 6 && xbox["back"] == 7 && xbox["rs"] == 8 && xbox["ls"] == 9,
        "Xbox: Menu, View, RSB, LSB");
  for (const auto& [k, v] : xbox) {
    Check(v < 10, "Xbox: the Xbox button and the shifter press nothing");
  }

  // Other wheels (unknown ones): face buttons, then the paddles.
  auto generic = WheelButtons(ButtonLayout::kGeneric, {});
  Check(generic["a"] == 0 && generic["b"] == 1 && generic["x"] == 2 && generic["y"] == 3,
        "generic face buttons");
  Check(generic["rb"] == 4 && generic["lb"] == 5, "generic paddles on the bumpers");

  // wheel_buttons overrides win; -1 unmaps; unknown names are ignored.
  auto m = WheelButtons(ButtonLayout::kPlayStation, {{"rb", 6}, {"lb", 7}, {"x", -1}, {"turbo", 3}});
  Check(m["rb"] == 6 && m["lb"] == 7 && m["x"] == -1, "overrides");
  Check(m.count("turbo") == 0 && m.size() == generic.size(), "unknown names ignored");

  // A button the shifter uses presses no pad button (#256: reverse on 11 was the left stick
  // press, which opened the friends overlay), whatever the defaults or the overrides say.
  const std::set<int> shifter = {11, 12, 13, 14, 15, 16, 17};
  auto freed = WheelButtons(ButtonLayout::kGeneric, {}, shifter);
  Check(freed["ls"] == -1, "generic: the shifter's reverse is no left stick press");
  Check(freed["rs"] == 10 && freed["start"] == 9 && freed["a"] == 0, "the rest is unchanged");
  auto forced = WheelButtons(ButtonLayout::kXbox, {{"ls", 11}, {"rs", 12}, {"y", 3}}, shifter);
  Check(forced["ls"] == -1 && forced["rs"] == -1, "an override onto a shifter button is dropped");
  Check(forced["y"] == 3, "other overrides stay");
  auto sequential = WheelButtons(ButtonLayout::kPlayStation, {{"up", 19}}, {19, 20});
  Check(sequential["up"] == -1, "sequential up/down buttons press nothing else either");
  for (const auto& [k, v] : WheelButtons(ButtonLayout::kPlayStation, {}, {})) {
    Check(v == g29[k], "nothing taken: the plain map");
  }

  // The hat is the d-pad (SDL hat bits up 1, right 2, down 4, left 8 to XInput up 1, down 2,
  // left 4, right 8) except for directions wheel_buttons sets: the launcher's Clear writes
  // up=-1, which must turn hat up off (#279), and a bound button replaces the hat's direction.
  Check(HatDpad(0x01, {}) == 0x0001 && HatDpad(0x02, {}) == 0x0008 && HatDpad(0x04, {}) == 0x0002 &&
            HatDpad(0x08, {}) == 0x0004 && HatDpad(0x03, {}) == 0x0009 && HatDpad(0, {}) == 0,
        "the hat is the d-pad");
  Check(HatDpad(0x01, ParseKvLike("up", -1)) == 0, "a cleared d-pad direction ignores the hat");
  Check(HatDpad(0x09, ParseKvLike("up", -1)) == 0x0004, "the other directions keep the hat");
  Check(HatDpad(0x01, ParseKvLike("up", 12)) == 0, "a bound button replaces the hat's direction");
  Check(HatDpad(0x01, ParseKvLike("a", -1)) == 0x0001, "other buttons leave the hat alone");
  std::puts("wheel buttons: all checks passed");
  return 0;
}
