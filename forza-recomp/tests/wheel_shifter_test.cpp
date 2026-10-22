// The racing wheel's shifter logic (SDK patch 0099, rex/input/wheel/wheel_shifter.h): gear button
// maps, the lever's gear and the realistic clutch mode. Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   g++ -std=c++17 -Wall -Wextra -Werror -I ~/rexglue-sdk-src-forza/sdk/include
//       forza-recomp/tests/wheel_shifter_test.cpp -o wheel-shifter-test && ./wheel-shifter-test
#include <rex/input/wheel/wheel_shifter.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

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
  // The Driving Force Shifter: gears 1-6 on buttons 12-17, reverse on 18; six forward gears.
  const ShifterMap df = LogitechShifter();
  for (int g = 1; g <= 6; g++) {
    Check(df.gear[g] == 11 + g, "Driving Force Shifter gear buttons");
  }
  Check(df.gear[0] == 18 && df.gear[7] == -1, "Driving Force Shifter reverse");
  Check(TopGear(df) == 6, "six forward gears");
  Check(TopGear(ShifterMap{}) == 0, "an empty map has no gears");

  // The built-in map by model (#256): reverse is 18 on the PlayStation wheels, 11 on the Xbox ones
  // (a G923 for Xbox owner's reverse); gears 1-6 are 12-17 on all of them.
  struct Model {
    uint16_t pid;
    int reverse;
  };
  for (const Model& model : {Model{0xC24F, 18}, Model{0xC266, 18}, Model{0xC267, 18},
                             Model{0xC262, 11}, Model{0xC26E, 11}, Model{0xC26D, 11}}) {
    const ShifterMap m = DefaultShifter(0x046D, model.pid);
    Check(m.gear[0] == model.reverse, "the model's reverse button");
    for (int g = 1; g <= 6; g++) {
      Check(m.gear[g] == 11 + g, "the model's gear buttons");
    }
    Check(m.gear[7] == -1 && m.up == -1 && m.down == -1 && TopGear(m) == 6, "six gears, no up/down");
  }
  for (uint16_t pid : {0xC29B, 0xC299, 0xC29A, 0xC26F}) {
    Check(TopGear(DefaultShifter(0x046D, pid)) == 0, "other Logitech wheels: no built-in shifter");
  }
  Check(TopGear(DefaultShifter(0x044F, 0xC26E)) == 0, "another vendor: no built-in shifter");
  // wheel_shifter_buttons still overrides the model's map.
  Check(ApplyShifterOverrides(DefaultShifter(0x046D, 0xC26E), {{"r", 18}}).gear[0] == 18,
        "an override wins over the model's reverse");

  // The buttons the shifter reads, which press no pad button (wheel_buttons.h).
  const std::set<int> xbox_h = ShifterButtons(DefaultShifter(0x046D, 0xC26E), "h");
  Check(xbox_h == std::set<int>{11, 12, 13, 14, 15, 16, 17}, "H-pattern: the gears and reverse");
  ShifterMap seq = ApplyShifterOverrides(df, {{"up", 20}, {"down", 21}});
  Check(ShifterButtons(seq, "sequential") == std::set<int>{20, 21}, "sequential: up and down");
  Check(ShifterButtons(seq, "off").empty(), "no shifter: none");
  Check(ShifterButtons(ApplyShifterOverrides(df, {{"r", -1}}), "h").count(-1) == 0,
        "an unmapped gear takes no button");

  // Overrides: gears, reverse, a seventh gear, unmapping, sequential up/down; junk is ignored.
  ShifterMap m = ApplyShifterOverrides(df, {{"r", 20}, {"7", 19}, {"6", -1}, {"up", 4},
                                            {"down", 5}, {"8", 30}, {"x", 1}, {"0", 2}});
  Check(m.gear[0] == 20 && m.gear[7] == 19 && m.gear[6] == -1, "gear overrides");
  Check(m.up == 4 && m.down == 5, "sequential overrides");
  Check(m.gear[1] == 12 && TopGear(m) == 7, "untouched gears and the top gear");

  // The lever: the held gear button; none, or two at once, is neutral.
  std::set<int> held;
  auto is_held = [&](int n) { return held.count(n) != 0; };
  Check(LeverGear(df, is_held) == kGearNeutral, "nothing held is neutral");
  for (int g = 1; g <= 6; g++) {
    held = {11 + g};
    Check(LeverGear(df, is_held) == g, "a gear button selects its gear");
  }
  held = {18};
  Check(LeverGear(df, is_held) == kGearReverse, "reverse");
  held = {12, 13};
  Check(LeverGear(df, is_held) == kGearNeutral, "two gears at once are neutral");
  held = {3, 9};
  Check(LeverGear(df, is_held) == kGearNeutral, "other buttons are not gears");

  // Arcade (no clutch needed): the lever's gear at once.
  Check(EngageGear(3, 1, false, 0.0f, 0.5f) == 3, "arcade engages without the clutch");
  Check(EngageGear(kGearReverse, 0, false, 0.0f, 0.5f) == kGearReverse, "arcade reverse");

  // Realistic: a new gear needs the clutch at the threshold; neutral never does; an engaged gear
  // stays in when the clutch comes up.
  Check(EngageGear(2, 0, true, 0.2f, 0.5f) == kGearNeutral, "no clutch, no gear");
  Check(EngageGear(2, 0, true, 0.5f, 0.5f) == 2, "the clutch at the threshold engages");
  Check(EngageGear(2, 2, true, 0.0f, 0.5f) == 2, "the gear stays in with the clutch up");
  Check(EngageGear(3, 2, true, 0.1f, 0.5f) == kGearNeutral, "a new gear without the clutch: neutral");
  Check(EngageGear(kGearNeutral, 3, true, 0.0f, 0.5f) == kGearNeutral, "neutral needs no clutch");
  Check(EngageGear(kGearReverse, 0, true, 0.9f, 0.5f) == kGearReverse, "reverse with the clutch");

  std::puts("wheel shifter: all checks passed");
  return 0;
}
