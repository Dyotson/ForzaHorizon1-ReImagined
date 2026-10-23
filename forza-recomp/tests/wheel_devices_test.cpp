// Pedals and shifters on their own USB device (SDK patch 0128, src/input/wheel/wheel_devices.h,
// #272): which joystick is the pedal set or the shifter, which device each pedal reads, a pedal
// device's axes and a known shifter's buttons. Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; g++ -std=c++17 -Wall -Wextra -Werror -I $SDK/include
//       -I $SDK/src/input/wheel forza-recomp/tests/wheel_devices_test.cpp -o wheel-devices-test
//   ./wheel-devices-test
#include <wheel_devices.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <utility>

using namespace rex::input::wheel;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

JoyInfo Joy(const char* name, uint16_t vid, uint16_t pid, bool sdl_wheel = false,
            const char* guid = "") {
  JoyInfo j;
  j.name = name;
  j.guid = guid;
  j.vid = vid;
  j.pid = pid;
  j.sdl_wheel = sdl_wheel;
  return j;
}
}  // namespace

int main() {
  // --- detection -----------------------------------------------------------------------------
  Check(DetectAuxRole(0x0483, 0x0525, "") == AuxRole::kPedals, "Simagic P1000 by id");
  Check(DetectAuxRole(0x3670, 0x0902, "") == AuxRole::kPedals, "Simagic P2000 (haptic) by id");
  Check(DetectAuxRole(0x0483, 0x0531, "") == AuxRole::kShifter, "Simagic DS-8X by id");
  Check(DetectAuxRole(0x0EB7, 0x183B, "") == AuxRole::kPedals, "Fanatec ClubSport V3 by id");
  Check(DetectAuxRole(0x0EB7, 0x1A92, "") == AuxRole::kShifter, "Fanatec shifter by id");
  Check(DetectAuxRole(0x0EB7, 0x1A93, "") == AuxRole::kOther, "Fanatec handbrake by id");
  Check(DetectAuxRole(0x044F, 0xB371, "") == AuxRole::kPedals, "T-LCM by id");
  Check(DetectAuxRole(0x30B7, 0x1003, "") == AuxRole::kPedals, "Heusinkveld Ultimate by id");
  Check(DetectAuxRole(0x346E, 0x001E, "") == AuxRole::kShifter, "Moza HGP by id");
  // Wheel bases are not pedals or shifters, whatever their name says.
  Check(DetectAuxRole(0x0483, 0x0522, "Simagic Alpha") == AuxRole::kNone, "Simagic base");
  Check(DetectAuxRole(0x3670, 0x0502, "SIMAGIC EVO PRO") == AuxRole::kNone, "Simagic EVO Pro");
  Check(DetectAuxRole(0x0EB7, 0x0011, "FANATEC CSR Elite") == AuxRole::kNone, "CSR Elite");
  Check(DetectAuxRole(0x046D, 0xC266, "Logitech G923") == AuxRole::kNone, "G923");
  // Names, for devices without a known id.
  Check(DetectAuxRole(0x1234, 1, "Thrustmaster TH8A Shifter") == AuxRole::kShifter, "TH8A name");
  Check(DetectAuxRole(0x1234, 1, "TH8A") == AuxRole::kShifter, "TH8A alone");
  Check(DetectAuxRole(0x346E, 0x7777, "Gudsen MOZA SR-P  Pedals") == AuxRole::kPedals,
        "Moza pedals by name");
  Check(DetectAuxRole(0x346E, 0x7778, "Gudsen MOZA R5 Racing Wheel and Pedals") == AuxRole::kNone,
        "a wheel and pedals bundle is a wheel");
  Check(DetectAuxRole(0x346E, 0x7779, "HBP Handbrake") == AuxRole::kOther, "handbrake by name");
  Check(DetectAuxRole(0x1234, 2, "Generic USB Joystick") == AuxRole::kNone, "generic joystick");
  // Every model has a role and a name, and no id twice.
  std::set<std::pair<uint16_t, uint16_t>> ids;
  for (const auto& m : kAuxModels) {
    Check(m.role != AuxRole::kNone && m.name && *m.name, "model role and name");
    Check(ids.insert({m.vid, m.pid}).second, "model ids are unique");
    if (m.axes) {
      Check(m.throttle >= 0 && m.throttle < m.axes && m.brake >= 0 && m.brake < m.axes &&
                m.clutch >= 0 && m.clutch < m.axes,
            "a pedal layout's axes are within its count");
    }
  }

  // --- settings and picking --------------------------------------------------------------------
  Check(ParseDeviceChoice("") == DeviceChoice::kAuto && ParseDeviceChoice(" Auto ") == DeviceChoice::kAuto,
        "empty and auto");
  Check(ParseDeviceChoice("none") == DeviceChoice::kNone && ParseDeviceChoice("Wheel") == DeviceChoice::kNone,
        "none and wheel");
  Check(ParseDeviceChoice("SIMAGIC P1000") == DeviceChoice::kNamed, "a name");
  const JoyInfo evo = Joy("SIMAGIC EVO PRO", 0x3670, 0x0502, true, "03000000703600000205000000000000");
  const JoyInfo p1000 = Joy("SIMAGIC P1000", 0x0483, 0x0525, false, "03000000830400002505000000000000");
  const JoyInfo ds8x = Joy("SIMAGIC DS-8X", 0x0483, 0x0531);
  const JoyInfo th8a = Joy("Thrustmaster TH8A", 0x044F, 0x1111);
  const JoyInfo box = Joy("Arduino Leonardo", 0x2341, 0x8036);
  Check(DeviceMatch("simagic  p1000", p1000) == 2, "name, any case and spacing");
  Check(DeviceMatch("03000000830400002505000000000000", p1000) == 2, "GUID");
  Check(DeviceMatch("0483:0525", p1000) == 2, "vid:pid");
  Check(DeviceMatch("P1000", p1000) == 1 && DeviceMatch("P2000", p1000) == 0, "part of the name");
  const JoyInfo vjoy = Joy("Shaul Eizikovich vJoy - Virtual Joystick", 0x1234, 0xBEAD);
  Check(DeviceMatch("vJoy Device", vjoy) == 2, "SDL2 vJoy name matches SDL3 by USB id");
  Check(DeviceMatch("vJoy Device", evo) == 0 &&
            DeviceMatch("vJoy Device", Joy(vjoy.name.c_str(), 0x1234, 1)) == 0,
        "vJoy alias does not match other devices");
  const AuxPick vjoy_pick = PickAuxDevices({evo, vjoy}, "vJoy Device", "vJoy Device");
  Check(vjoy_pick.pedals == 1 && vjoy_pick.shifter == 1,
        "existing vJoy bindings pick the actual auxiliary device");

  // Simagic EVO Pro with P1000 pedals and a DS-8X: pedals and shifter found by id.
  AuxPick pick = PickAuxDevices({evo, p1000, ds8x}, "", "");
  Check(pick.pedals == 1 && pick.shifter == 2 && pick.other.empty(), "Simagic set, auto");
  Check(!pick.IsAux(0) && pick.IsAux(1) && pick.IsAux(2), "the base stays a wheel");
  // A TH8A (no known id) by its name; none turns auto-detection off.
  pick = PickAuxDevices({evo, th8a}, "", "");
  Check(pick.pedals == -1 && pick.shifter == 1, "TH8A by name");
  pick = PickAuxDevices({evo, p1000, ds8x}, "none", "none");
  Check(pick.pedals == -1 && pick.shifter == -1, "none reads the wheel");
  Check(pick.other.size() == 2 && pick.IsAux(1) && pick.IsAux(2),
        "with none, pedals and shifter are still not wheels of their own");
  // A named device the detection does not know (an Arduino button box as the shifter).
  pick = PickAuxDevices({evo, box, p1000}, "", "Arduino Leonardo");
  Check(pick.shifter == 1 && pick.pedals == 2, "a named shifter");
  // "Simagic" matches the base and the pedals by part of the name: the one SDL does not list as a
  // wheel base wins; an exact name wins over a part.
  pick = PickAuxDevices({evo, p1000}, "simagic", "");
  Check(pick.pedals == 1, "a partial name prefers a non-base device");
  pick = PickAuxDevices({Joy("Pedals X", 1, 1), Joy("Pedals", 1, 2)}, "pedals", "none");
  Check(pick.pedals == 1, "an exact name wins");
  // A named device that is not connected: no pedal device, and detected pedals are not used.
  pick = PickAuxDevices({evo, p1000}, "Heusinkveld Sprint", "");
  Check(pick.pedals == -1 && pick.IsAux(1), "named but missing");
  // One device can be both.
  pick = PickAuxDevices({evo, box}, "Arduino Leonardo", "Arduino Leonardo");
  Check(pick.pedals == 1 && pick.shifter == 1, "one device for both");
  // Two pedal sets: the first is used, the second is no wheel either.
  pick = PickAuxDevices({p1000, evo, Joy("FANATEC ClubSport Pedals V3", 0x0EB7, 0x183B)}, "", "");
  Check(pick.pedals == 0 && pick.other.size() == 1 && pick.other[0] == 2, "second pedal set");

  // --- which device each pedal reads ---------------------------------------------------------
  std::map<std::string, int> none, wheel_thr{{"throttle", 2}}, pedal_clutch{{"clutch", 0}},
      unset{{"throttle", -1}};
  PedalSources s = PlanPedals(none, none, false);
  Check(!s.Any(), "no pedal device: the wheel, as before");
  s = PlanPedals(none, none, true);
  Check(s.from_aux[0] && s.from_aux[1] && s.from_aux[2], "a pedal device gives every pedal");
  s = PlanPedals(wheel_thr, none, true);
  Check(!s.from_aux[0] && s.from_aux[1] && s.from_aux[2], "wheel_axes keeps a pedal on the wheel");
  s = PlanPedals(wheel_thr, std::map<std::string, int>{{"throttle", 1}}, true);
  Check(s.from_aux[0], "wheel_pedal_axes wins over wheel_axes");
  s = PlanPedals(none, pedal_clutch, false);
  Check(!s.from_aux[0] && !s.from_aux[1] && s.from_aux[2],
        "a pedal set on the pedal device reads it even while it is unplugged (nothing)");
  s = PlanPedals(unset, none, true);
  Check(s.from_aux[0], "a negative axis is the default");

  // --- a pedal device's axes -----------------------------------------------------------------
  const bool all[3] = {true, true, true};
  int axes[3];
  // Simagic P1000 with its three axes: Rz throttle (2), Ry brake (1), Rx clutch (0).
  PedalDeviceAxes(axes, all, none, FindAuxModel(0x0483, 0x0525), {-32768, -32768, -32768}, false);
  Check(axes[0] == 2 && axes[1] == 1 && axes[2] == 0, "P1000 layout");
  // The same pedals with a fourth axis: the layout does not hold, so detection, in axis order.
  PedalDeviceAxes(axes, all, none, FindAuxModel(0x0483, 0x0525), {-32768, -32768, -32768, 0},
                  false);
  Check(axes[0] == 0 && axes[1] == 1 && axes[2] == 2, "layout dropped for another axis count");
  // T-LCM: X throttle, Y clutch, Z brake.
  PedalDeviceAxes(axes, all, none, FindAuxModel(0x044F, 0xB371), {32767, 32767, 32767}, false);
  Check(axes[0] == 0 && axes[1] == 2 && axes[2] == 1, "T-LCM layout");
  // Unknown pedals: detected where they rest at an end; a centred axis is skipped.
  PedalDeviceAxes(axes, all, none, nullptr, {0, 32767, -32768}, false);
  Check(axes[0] == 1 && axes[1] == 2 && axes[2] == -1, "detected pedals");
  // wheel_pedal_axes first, then detection fills the rest around it.
  PedalDeviceAxes(axes, all, std::map<std::string, int>{{"brake", 0}}, nullptr,
                  {-32768, -32768, -32768}, false);
  Check(axes[1] == 0 && axes[0] == 1 && axes[2] == 2, "an override and detection");
  // Only the pedals this device gives.
  const bool clutch_only[3] = {false, false, true};
  PedalDeviceAxes(axes, clutch_only, none, nullptr, {-32768, -32768}, false);
  Check(axes[0] == -1 && axes[1] == -1 && axes[2] == 0, "only the clutch");
  // Combined: the first axis even at the centre.
  PedalDeviceAxes(axes, all, none, nullptr, {0, -32768}, true);
  Check(axes[0] == 0 && axes[1] == 1, "combined pedal axis");
  // The wheel's own detection is unchanged: steer skipped, pedals at the ends.
  int wheel_pedals[3] = {-1, -1, -1};
  DetectPedalAxes(wheel_pedals, all, 0, {0, 32767, 32767, 32767}, false);
  Check(wheel_pedals[0] == 1 && wheel_pedals[1] == 2 && wheel_pedals[2] == 3, "wheel detection");
  const bool no_throttle[3] = {false, true, true};
  int some[3] = {-1, -1, -1};
  DetectPedalAxes(some, no_throttle, 0, {0, 32767, 32767}, false);
  Check(some[0] == -1 && some[1] == 1 && some[2] == 2, "a pedal from the pedal device is skipped");

  // --- shifters ------------------------------------------------------------------------------
  ShifterMap m = AuxShifterMap(FindAuxModel(0x0EB7, 0x1A92), none);
  Check(m.gear[0] == 0 && m.gear[1] == 1 && m.gear[7] == 7 && m.up == 9 && m.down == 8,
        "Fanatec ClubSport Shifter SQ");
  m = AuxShifterMap(FindAuxModel(0x0483, 0x0531), none);
  Check(m.gear[1] == 0 && m.gear[7] == 6 && m.gear[0] == 8 && TopGear(m) == 7, "Simagic DS-8X");
  m = AuxShifterMap(FindAuxModel(0x346E, 0x001E), none);
  Check(m.gear[0] == 4 && m.gear[1] == 5 && m.gear[7] == 11, "Moza HGP");
  m = AuxShifterMap(FindAuxModel(0x046D, 0xC273), none);
  Check(m.gear[1] == 0 && m.gear[6] == 5 && m.gear[7] == -1 && m.gear[0] == 6,
        "Driving Force Shifter on the USB adapter");
  m = AuxShifterMap(nullptr, none);
  Check(m.gear[1] == -1 && TopGear(m) == 0, "unknown shifter without buttons");
  m = AuxShifterMap(FindAuxModel(0x0483, 0x0531), {{"1", 3}, {"r", 9}});
  Check(m.gear[1] == 3 && m.gear[0] == 9 && m.gear[2] == 1, "overrides on a known shifter");
  int held_button = 2;
  Check(LeverGear(AuxShifterMap(FindAuxModel(0x0483, 0x0531), none),
                  [&](int n) { return n == held_button; }) == 3,
        "DS-8X button 2 is third gear");

  std::puts("wheel_devices_test: ok");
  return 0;
}
