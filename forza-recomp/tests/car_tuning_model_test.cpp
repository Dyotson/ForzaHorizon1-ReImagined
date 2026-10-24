#include "../src/platform/car_tuning_model.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace forza::car_tuning;
namespace {
void Check(bool value, const char* what) {
  if (!value) { std::cerr << what << '\n'; std::exit(1); }
}
Payload Stock() {
  Payload result;
  result.fill(std::bit_cast<uint32_t>(kStock));
  // Unknown fields must survive unchanged, including values not representable
  // as finite floats. The pressure editor has no authority to normalize them.
  result[0] = 0x7FC01234;
  result[46] = 0x80000000;
  return result;
}
}

int main() {
  const Identity car{17, 900, 0x100000, 0x200000};
  const PressureLimits limits{15, 55, 30};
  const auto original = Stock();
  EditSession edit;
  Check(edit.Begin(car, original, limits), "valid stock session");
  Check(edit.IsStock(Axle::Front) && edit.Pressure(Axle::Front) == 30, "stock resolves for display");
  Check(!edit.dirty(), "display does not normalize the save");
  Check(edit.AdjustPressure(Axle::Front, 0), "zero adjustment");
  Check(!edit.dirty() && edit.IsStock(Axle::Front), "zero adjustment preserves stock");
  Check(edit.SetPressure(Axle::Front, 31.5), "front pressure edit");
  Check(edit.Pressure(Axle::Front) == 31.5f && edit.dirty(), "front pressure round trip");
  for (size_t i = 0; i < original.size(); ++i)
    Check(i == PressureWord(Axle::Front) || edit.pending()[i] == original[i], "unrelated tune words preserved");
  Check(edit.IsStock(Axle::Rear), "rear stock stays stock");
  Check(!edit.SetPressure(Axle::Rear, 14.99) && !edit.SetPressure(Axle::Rear, 55.01), "range rejection");
  Check(!edit.SetPressure(Axle::Rear, std::numeric_limits<double>::quiet_NaN()), "NaN rejection");
  Check(!edit.AdjustPressure(Axle::Rear, std::numeric_limits<double>::infinity()), "infinite adjustment rejection");
  Check(edit.AdjustPressure(Axle::Front, 10000) && edit.Pressure(Axle::Front) == 55, "step clamps at upper bound");
  Check(edit.AdjustPressure(Axle::Front, -10000) && edit.Pressure(Axle::Front) == 15, "step clamps at lower bound");
  Check(edit.CheckCommit(car, original, limits) == CommitCheck::Ready, "ready transaction");
  auto changed_car = car; changed_car.owned_id++;
  Check(edit.CheckCommit(changed_car, original, limits) == CommitCheck::CarChanged, "different garage row rejected");
  changed_car = car; changed_car.live_car++;
  Check(edit.CheckCommit(changed_car, original, limits) == CommitCheck::CarChanged, "reloaded car rejected");
  changed_car = car; changed_car.definition++;
  Check(edit.CheckCommit(changed_car, original, limits) == CommitCheck::CarChanged, "replaced definition rejected");
  changed_car = car; changed_car.parts[31]++;
  Check(edit.CheckCommit(changed_car, original, limits) == CommitCheck::CarChanged, "upgrade replacement with unchanged tune rejected");
  auto changed_data = original; changed_data[1] ^= 1;
  Check(edit.CheckCommit(car, changed_data, limits) == CommitCheck::DataChanged, "concurrent upgrade/tune rejected");
  auto changed_limits = limits; changed_limits.maximum++;
  Check(edit.CheckCommit(car, original, changed_limits) == CommitCheck::LimitsChanged, "changed native limits rejected");
  Check(!edit.AcknowledgeApplied(car, original), "failed persistence not acknowledged");
  auto applied = edit.pending();
  Check(edit.AcknowledgeApplied(car, applied) && !edit.dirty(), "matching native write acknowledged");
  Check(edit.ResetPressure(Axle::Front) && edit.IsStock(Axle::Front), "reset uses native sentinel");
  edit.Cancel();
  Check(edit.pending() == applied && !edit.dirty(), "cancel restores original bits");
  edit.Close();
  Check(!edit.SetPressure(Axle::Front, 25) && !edit.Pressure(Axle::Front), "closed session refuses edits");
  Check(edit.CheckCommit(car, original, limits) == CommitCheck::Closed, "closed commit refused");
  for (float invalid : {-2.0f, 1.01f, std::numeric_limits<float>::quiet_NaN()}) {
    auto bad = original; bad[PressureWord(Axle::Front)] = std::bit_cast<uint32_t>(invalid);
    Check(!edit.Begin(car, bad, limits), "invalid saved pressure refused");
  }
  Check(!edit.Begin({}, original, limits), "unknown car refused");
  changed_car = car; changed_car.definition = 0;
  Check(!edit.Begin(changed_car, original, limits), "missing definition refused");
  Check(!edit.Begin(car, original, {55, 15, 30}), "reversed limits refused");
  Check(!edit.Begin(car, original, {15, 55, 60}), "invalid native default refused");
  Check(edit.Begin(car, original, {10, 80, 29}), "loaded limits are not hardcoded");
  Check(edit.SetPressure(Axle::Rear, 80) && edit.Pressure(Axle::Rear) == 80, "custom loaded limit applied");
  auto schema = PressureSchema(limits);
  schema[size_t(Field::FinalDrive)] = FieldRange{2.2f, 6.1f, 3.5f};
  schema[size_t(Field::BrakeBalance)] = FieldRange{100, 0, 50};
  Check(edit.BeginSchema(car, original, schema), "multiple categories share one transaction");
  Check(edit.SetValue(Field::FinalDrive, 4.0) && edit.SetValue(Field::BrakeBalance, 60), "bounded original field edits");
  Check(edit.Value(Field::BrakeBalance) == 60, "decreasing native display direction supported");
  Check(!edit.SetValue(Field::Gear8, 1.2) && !edit.Reset(Field::RearAero), "locked or unsupported fields cannot be changed");
  Check(edit.CheckCommitSchema(car, original, schema) == CommitCheck::Ready, "schema transaction ready");
  auto changed_schema = schema; changed_schema[size_t(Field::FinalDrive)].reset();
  Check(edit.CheckCommitSchema(car, original, changed_schema) == CommitCheck::LimitsChanged, "lost upgrade capability invalidates apply");
  changed_schema = schema; changed_schema[size_t(Field::FinalDrive)]->at_one = 7;
  Check(edit.CheckCommitSchema(car, original, changed_schema) == CommitCheck::LimitsChanged, "car-specific range change invalidates apply");
  Check(edit.Reset(Field::FinalDrive) && edit.IsStock(Field::FinalDrive), "per-field reset preserves native stock semantics");
  edit.Cancel();
  Check(edit.pending() == original, "cancel restores every category and opaque word");
  schema[size_t(Field::FrontPressure)].reset();
  Check(edit.BeginSchema(car, original, schema) && !edit.ResetPressure(Axle::Front) &&
        !edit.SetPressure(Axle::Front, 35), "legacy pressure helpers respect generic capabilities");
  Check(!edit.BeginSchema(car, original, {}), "no editable fields refused");
  schema = {};
  schema[size_t(Field::FinalDrive)] = FieldRange{-std::numeric_limits<float>::max(),
                                               std::numeric_limits<float>::max(), 0};
  Check(edit.BeginSchema(car, original, schema) && edit.SetValue(Field::FinalDrive, 0) &&
        edit.Value(Field::FinalDrive) == 0 &&
        std::bit_cast<float>(edit.pending()[FieldWord(Field::FinalDrive)]) == 0.5f,
        "finite wide endpoint arithmetic does not overflow");
  schema = {};
  schema[size_t(Field::RearAcceleration)] = FieldRange{0, 100, 30, Field::RearDeceleration};
  Check(edit.BeginSchema(car, original, schema), "sport differential linked field session");
  Check(!edit.SetValue(Field::RearDeceleration, 40), "linked coast field is not directly editable");
  Check(edit.SetValue(Field::RearAcceleration, 65), "sport rear acceleration edit");
  Check(edit.pending()[FieldWord(Field::RearAcceleration)] ==
        edit.pending()[FieldWord(Field::RearDeceleration)], "sport rear differential preserves native normalized coupling");
  for (size_t i = 0; i < original.size(); ++i)
    Check(i == FieldWord(Field::RearAcceleration) || i == FieldWord(Field::RearDeceleration) ||
          edit.pending()[i] == original[i], "linked edit preserves all unrelated words");
  Check(edit.Reset(Field::RearAcceleration) && edit.pending() == original, "linked reset restores both stock sentinels");
  changed_schema = schema;
  changed_schema[size_t(Field::RearAcceleration)]->linked_normalized.reset();
  Check(edit.CheckCommitSchema(car, original, changed_schema) == CommitCheck::LimitsChanged,
        "coupling capability changes invalidate apply");
  schema[size_t(Field::RearDeceleration)] = FieldRange{0, 100, 20};
  Check(!edit.BeginSchema(car, original, schema), "ambiguous editable linked peer refused");
  schema = {};
  schema[size_t(Field::Gear1)] = FieldRange{0.5f, 6, 4};
  schema[size_t(Field::Gear2)] = FieldRange{0.5f, 6, 3};
  schema[size_t(Field::Gear3)] = FieldRange{0.5f, 6, 2};
  Check(edit.BeginSchema(car, original, schema), "ordered gear session");
  Check(!edit.SetValue(Field::Gear2, 4.5) && !edit.SetValue(Field::Gear2, 1.5) && !edit.dirty(),
        "crossing either adjacent gear rejected without mutation");
  Check(edit.SetValue(Field::Gear2, 3.5), "ordered gear edit");
  Check(edit.SetValue(Field::Gear1, 3.5) && edit.SetValue(Field::Gear2, 2) &&
        edit.SetValue(Field::Gear1, 2), "equal adjacent gears allowed");
  Check(!edit.Reset(Field::Gear2), "individual stock reset cannot cross adjusted neighbor");
  edit.ResetAll();
  Check(edit.pending() == original, "whole setup reset restores stock gear order atomically");
  std::cout << "car tuning model: passed\n";
}
