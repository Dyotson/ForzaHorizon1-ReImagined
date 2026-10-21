// Standalone: c++ -std=c++17 graphics_presets_test.cpp -o graphics_presets_test
#include "../src/platform/graphics_presets.h"
#include <cassert>
int main() {
  using forza::graphics_menu::ChoosePreset;
  assert(ChoosePreset("auto", "1080p", true, true).mode == "off");
  assert(ChoosePreset("auto", "1440p", true, true).mode == "quality");
  assert(ChoosePreset("auto", "4k", false, true).mode == "fsr3_quality");
  assert(ChoosePreset("low", "1440p", true, true).mode == "dlss");
  assert(ChoosePreset("medium", "1440p", true, true).mode == "balanced");
  const auto native = ChoosePreset("high", "1440p", false, false);
  assert(native.mode == "off" && native.scene_scale == 2 && native.filtering == 5);
  const auto low = ChoosePreset("low", "4k", false, false);
  assert(low.scene_scale == 1 && low.filtering == 2);
}
