// The cockpit gauge backlight (src/platform/cockpit_gauges.h, #183): which models are the
// cockpit, where the headlight amount sits in the constant shadow, and the lit amount.
// Standalone: c++ -std=c++17 cockpit_gauges_test.cpp -o cockpit_gauges_test
#include "../src/platform/cockpit_gauges.h"
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <limits>

int main() {
  namespace g = forza::cockpit_gauges;
  // The needles (1-15) and the cockpit (16); none of the car's other models (0, 17+).
  for (uint32_t group = 1; group <= 16; ++group) assert(g::IsCockpitGroup(group));
  for (uint32_t group : {0u, 17u, 18u, 20u, 21u, 22u, 40u, 41u, 61u, 62u, 0xFFFFFFFFu})
    assert(!g::IsCockpitGroup(group));

  // What sub_82448F90 writes: c58 at +6944 (dirty bit 49), c108 at +7744 (bit 36,
  // `rldicr r11,r11,36,63`), the c108/c109 pair under one bit.
  assert(g::PixelConstantOffset(58) == 6944);
  assert(g::PixelConstantOffset(109) == 7760);
  assert(g::kHeadlightAmountOffset == 7744);
  assert(g::PixelConstantDirtyBit(58) == uint64_t(1) << 49);
  assert(g::PixelConstantDirtyBit(108) == uint64_t(1) << 36);
  assert(g::PixelConstantDirtyBit(109) == g::PixelConstantDirtyBit(108));
  assert(g::PixelConstantDirtyBit(112) == uint64_t(1) << 35);

  // Lit by day, the game's own amount at night, never the garbage of a broken value.
  assert(g::LitHeadlightAmount(0.0f) == 1.0f);
  assert(g::LitHeadlightAmount(0.4f) == 1.0f);
  assert(g::LitHeadlightAmount(1.0f) == 1.0f);
  assert(g::LitHeadlightAmount(1.5f) == 1.5f);
  assert(g::LitHeadlightAmount(-2.0f) == 1.0f);
  assert(g::LitHeadlightAmount(std::numeric_limits<float>::quiet_NaN()) == 1.0f);
  assert(g::LitHeadlightAmount(std::numeric_limits<float>::infinity()) == 1.0f);
}
