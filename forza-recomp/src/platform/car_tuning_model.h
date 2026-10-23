// Original editing model for FH1-owned tune data. No game assets or runtime needed.
#pragma once

#include "car_tuning_fields.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace forza::car_tuning {

// Preserve every word, including fields this version does not expose. The native
// garage format stores normalized values; -1 selects the native stock default.
using Payload = std::array<uint32_t, 47>;
inline constexpr float kStock = -1.0f;
enum class Axle { Front, Rear };
inline constexpr size_t PressureWord(Axle axle) {
  return axle == Axle::Front ? 0x30 / 4 : 0x5C / 4;
}

struct Identity {
  uint32_t owned_id = 0;
  uint32_t model_id = 0;
  uint32_t live_car = 0;
  uint32_t definition = 0;
  // Native setup's 45 part IDs precede its tune payload. Compare them exactly:
  // replacing upgrades can preserve both the live address and tune values.
  std::array<uint32_t, 45> parts{};
  bool operator==(const Identity&) const = default;
  explicit operator bool() const { return owned_id && model_id && live_car && definition; }
};

// Supply limits from the loaded FH1 physics settings, not from host constants.
struct PressureLimits {
  float minimum = 0;
  float maximum = 0;
  float stock = 0;
  bool operator==(const PressureLimits&) const = default;
  bool Valid() const {
    return std::isfinite(minimum) && std::isfinite(maximum) &&
           std::isfinite(stock) && minimum > 0 && maximum > minimum &&
           stock >= minimum && stock <= maximum;
  }
};

inline FieldSchema PressureSchema(PressureLimits limits) {
  FieldSchema schema{};
  if (limits.Valid()) {
    schema[size_t(Field::FrontPressure)] = schema[size_t(Field::RearPressure)] =
        FieldRange{limits.minimum, limits.maximum, limits.stock};
  }
  return schema;
}

enum class CommitCheck {
  Ready, Unchanged, Closed, CarChanged, DataChanged, LimitsChanged
};

class EditSession {
 public:
  bool Begin(Identity identity, const Payload& payload, PressureLimits limits) {
    if (!limits.Valid()) { Close(); return false; }
    if (!BeginSchema(identity, payload, PressureSchema(limits))) return false;
    limits_ = limits;
    return true;
  }
  bool BeginSchema(Identity identity, const Payload& payload, const FieldSchema& schema) {
    Close();
    if (!identity) return false;
    bool any = false;
    for (size_t i = 0; i < schema.size(); ++i) {
      if (!schema[i]) continue;
      any = true;
      if (!schema[i]->Valid()) return false;
      if (const auto peer = schema[i]->linked_normalized) {
        if (size_t(*peer) >= kFieldCount || size_t(*peer) == i || schema[size_t(*peer)]) return false;
        for (size_t j = 0; j < i; ++j)
          if (schema[j] && schema[j]->linked_normalized == peer) return false;
        const auto linked = std::bit_cast<float>(payload[FieldWord(*peer)]);
        if (!std::isfinite(linked) || (linked != kStock && (linked < 0 || linked > 1))) return false;
      }
      const auto value = std::bit_cast<float>(payload[FieldWord(Field(i))]);
      if (!std::isfinite(value) ||
          (value != kStock && (value < 0 || value > 1))) return false;
    }
    if (!any) return false;
    identity_ = identity;
    original_ = pending_ = payload;
    schema_ = schema;
    limits_ = {};
    open_ = true;
    return true;
  }

  bool open() const { return open_; }
  bool dirty() const { return open_ && pending_ != original_; }
  const Identity& identity() const { return identity_; }
  const Payload& original() const { return original_; }
  const Payload& pending() const { return pending_; }
  const PressureLimits& limits() const { return limits_; }
  const FieldSchema& schema() const { return schema_; }
  bool Editable(Field field) const { return open_ && size_t(field) < kFieldCount && schema_[size_t(field)].has_value(); }
  bool IsStock(Field field) const {
    return Editable(field) && std::bit_cast<float>(pending_[FieldWord(field)]) == kStock;
  }
  std::optional<float> Value(Field field) const {
    if (!Editable(field)) return {};
    const auto& range = *schema_[size_t(field)];
    const auto value = std::bit_cast<float>(pending_[FieldWord(field)]);
    return value == kStock ? range.stock : float(double(range.at_zero) +
        double(value) * (double(range.at_one) - range.at_zero));
  }
  bool SetValue(Field field, double value) {
    if (!Editable(field)) return false;
    const auto& range = *schema_[size_t(field)];
    if (!std::isfinite(value) || value < range.Minimum() || value > range.Maximum()) return false;
    if (!GearOrderAllows(field, value)) return false;
    const float normalized = float((value - range.at_zero) / (double(range.at_one) - range.at_zero));
    SetNormalized(field, normalized);
    return true;
  }
  bool Reset(Field field) {
    if (!Editable(field)) return false;
    if (!GearOrderAllows(field, schema_[size_t(field)]->stock)) return false;
    SetNormalized(field, kStock);
    return true;
  }
  void ResetAll() {
    if (!open_) return;
    for (size_t i = 0; i < kFieldCount; ++i)
      if (Editable(Field(i))) SetNormalized(Field(i), kStock);
  }
  bool IsStock(Axle axle) const {
    return IsStock(axle == Axle::Front ? Field::FrontPressure : Field::RearPressure);
  }
  std::optional<float> Pressure(Axle axle) const {
    return Value(axle == Axle::Front ? Field::FrontPressure : Field::RearPressure);
  }

  // A typed value outside the native range is an error, not an invisible clamp.
  bool SetPressure(Axle axle, double psi) {
    return SetValue(axle == Axle::Front ? Field::FrontPressure : Field::RearPressure, psi);
  }
  bool AdjustPressure(Axle axle, double delta) {
    const auto current = Pressure(axle);
    if (!current || !std::isfinite(delta)) return false;
    if (delta == 0) return true;
    const auto& range = *schema_[size_t(axle == Axle::Front ? Field::FrontPressure : Field::RearPressure)];
    const double next = std::fmax(range.Minimum(),
        std::fmin(range.Maximum(), double(*current) + delta));
    if (next == *current) return true;
    return SetPressure(axle, next);
  }
  bool ResetPressure(Axle axle) {
    return Reset(axle == Axle::Front ? Field::FrontPressure : Field::RearPressure);
  }
  void Cancel() { if (open_) pending_ = original_; }
  void Close() { open_ = false; identity_ = {}; }

  // Recheck immediately before applying. Car swaps, upgrades and another writer
  // invalidate the session even if they happened while this page was suspended.
  CommitCheck CheckCommit(Identity identity, const Payload& current,
                          PressureLimits limits) const {
    return CheckCommitSchema(identity, current, PressureSchema(limits));
  }
  CommitCheck CheckCommitSchema(Identity identity, const Payload& current,
                                const FieldSchema& schema) const {
    if (!open_) return CommitCheck::Closed;
    if (identity != identity_) return CommitCheck::CarChanged;
    if (current != original_) return CommitCheck::DataChanged;
    if (schema != schema_) return CommitCheck::LimitsChanged;
    return dirty() ? CommitCheck::Ready : CommitCheck::Unchanged;
  }
  bool AcknowledgeApplied(Identity identity, const Payload& stored) {
    if (!open_ || identity != identity_ || stored != pending_) return false;
    original_ = stored;
    return true;
  }

 private:
  void SetNormalized(Field field, float value) {
    const auto bits = std::bit_cast<uint32_t>(value);
    pending_[FieldWord(field)] = bits;
    if (const auto peer = schema_[size_t(field)]->linked_normalized)
      pending_[FieldWord(*peer)] = bits;
  }
  bool GearOrderAllows(Field field, double value) const {
    if (field < Field::Gear1 || field > Field::Gear8) return true;
    // Equal adjacent ratios are supported by the native editor. Keep explicit
    // edits ordered without silently changing another visible gear's setting.
    if (field > Field::Gear1)
      if (const auto previous = Value(Field(size_t(field) - 1)); previous && value > *previous) return false;
    if (field < Field::Gear8)
      if (const auto next = Value(Field(size_t(field) + 1)); next && value < *next) return false;
    return true;
  }
  bool open_ = false;
  Identity identity_;
  PressureLimits limits_;
  FieldSchema schema_{};
  Payload original_{};
  Payload pending_{};
};

} // namespace forza::car_tuning
