// forza - the cockpit gauge backlight's pieces that need no game (src/hooks/cockpit_gauges.cpp).
#pragma once
#include <cmath>
#include <cstdint>

namespace forza::cockpit_gauges {
// The car models the game draws one by one (sub_8246CEC8's group): 16 is the cockpit
// interior with the gauge faces, 1-15 the analog gauge needles (sub_8248A5A0's table).
// The lamps, body and wheels are other groups.
constexpr bool IsCockpitGroup(uint32_t group) { return group >= 1 && group <= 16; }

// The shader constant shadow the car draws from: pixel shader register r at
// (r + 376) * 16, its dirty flag at bit 63 - r / 4 of the 64-bit word at +8 (the vertex
// shader's at +0, registers at (r + 120) * 16).
constexpr uint32_t PixelConstantOffset(uint32_t reg) { return (reg + 376) * 16; }
constexpr uint64_t PixelConstantDirtyBit(uint32_t reg) { return uint64_t(1) << (63 - reg / 4); }

// dynamicLights2 (c108): x is the car's headlight amount, which every lamp lens and the
// cockpit's gauge faces multiply their glow by.
constexpr uint32_t kDynamicLights2 = 108;
constexpr uint32_t kHeadlightAmountOffset = PixelConstantOffset(kDynamicLights2);

// The amount the cockpit is drawn with: the game's own once its lights are on (night),
// else full.
inline float LitHeadlightAmount(float game) {
  return std::isfinite(game) && game > 1.0f ? game : 1.0f;
}
}  // namespace forza::cockpit_gauges
