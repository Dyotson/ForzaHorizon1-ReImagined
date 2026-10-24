// Original field schema for FH1's local garage tuning interface.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace forza::car_tuning {

enum class Category { Tires, Gearing, Alignment, AntiRoll, Springs, Damping, Aero, Brakes, Differential };
enum class Field {
  FrontPressure, RearPressure, FinalDrive, Gear1, Gear2, Gear3, Gear4, Gear5,
  Gear6, Gear7, Gear8, FrontCamber, RearCamber, FrontToe, RearToe, Caster,
  FrontAntiRoll, RearAntiRoll, FrontSpring, RearSpring, FrontHeight, RearHeight,
  FrontRebound, RearRebound, FrontBump, RearBump, FrontAero, RearAero,
  BrakeBalance, BrakePressure, FrontAcceleration, FrontDeceleration,
  RearAcceleration, RearDeceleration, CenterBalance, Count
};
inline constexpr size_t kFieldCount = size_t(Field::Count);
struct FieldInfo { Category category; uint32_t offset; const char* label; };

// These are interface offsets within FH1's 0xBC-byte garage tune payload.
// Unlisted words are opaque and must survive edits unchanged.
inline constexpr std::array<FieldInfo, kFieldCount> kFields{{
  {Category::Tires, 0x30, "Front pressure"}, {Category::Tires, 0x5C, "Rear pressure"},
  {Category::Gearing, 0x08, "Final drive"},
  {Category::Gearing, 0x9C, "First gear"}, {Category::Gearing, 0xA0, "Second gear"},
  {Category::Gearing, 0xA4, "Third gear"}, {Category::Gearing, 0xA8, "Fourth gear"},
  {Category::Gearing, 0xAC, "Fifth gear"}, {Category::Gearing, 0xB0, "Sixth gear"},
  {Category::Gearing, 0xB4, "Seventh gear"}, {Category::Gearing, 0xB8, "Eighth gear"},
  {Category::Alignment, 0x34, "Front camber"}, {Category::Alignment, 0x60, "Rear camber"},
  {Category::Alignment, 0x38, "Front toe"}, {Category::Alignment, 0x64, "Rear toe"},
  {Category::Alignment, 0x3C, "Front caster"},
  {Category::AntiRoll, 0x44, "Front anti-roll bar"}, {Category::AntiRoll, 0x70, "Rear anti-roll bar"},
  {Category::Springs, 0x40, "Front spring"}, {Category::Springs, 0x6C, "Rear spring"},
  {Category::Springs, 0x48, "Front ride height"}, {Category::Springs, 0x74, "Rear ride height"},
  {Category::Damping, 0x4C, "Front rebound"}, {Category::Damping, 0x78, "Rear rebound"},
  {Category::Damping, 0x50, "Front bump"}, {Category::Damping, 0x7C, "Rear bump"},
  {Category::Aero, 0x00, "Front downforce"}, {Category::Aero, 0x04, "Rear downforce"},
  {Category::Brakes, 0x10, "Brake balance"}, {Category::Brakes, 0x0C, "Brake pressure"},
  {Category::Differential, 0x54, "Front acceleration"},
  {Category::Differential, 0x58, "Front deceleration"},
  {Category::Differential, 0x80, "Rear acceleration"},
  {Category::Differential, 0x84, "Rear deceleration"},
  {Category::Differential, 0x18, "Center balance"},
}};

inline constexpr size_t FieldWord(Field field) { return kFields[size_t(field)].offset / 4; }
consteval bool UniqueFieldOffsets() {
  for (size_t i = 0; i < kFields.size(); ++i) {
    if (kFields[i].offset >= 0xBC || kFields[i].offset % 4) return false;
    for (size_t j = 0; j < i; ++j) if (kFields[i].offset == kFields[j].offset) return false;
  }
  return true;
}
static_assert(UniqueFieldOffsets());

// Endpoints use the native display direction. A decreasing mapping is valid
// (for example front/rear balance); the adapter determines units and direction.
struct FieldRange {
  float at_zero = 0, at_one = 0, stock = 0;
  // Some native parts expose one control for two normalized fields. The peer
  // remains unavailable for direct editing; an explicit edit/reset updates both.
  std::optional<Field> linked_normalized = {};
  bool operator==(const FieldRange&) const = default;
  float Minimum() const { return std::fmin(at_zero, at_one); }
  float Maximum() const { return std::fmax(at_zero, at_one); }
  bool Valid() const {
    return std::isfinite(at_zero) && std::isfinite(at_one) && std::isfinite(stock) &&
           at_zero != at_one && stock >= Minimum() && stock <= Maximum();
  }
};
// Absence means not editable: unsupported, missing an upgrade, wrong drivetrain,
// or a native fixed range. Keeping a value visible does not grant write access.
using FieldSchema = std::array<std::optional<FieldRange>, kFieldCount>;

} // namespace forza::car_tuning
