// The analog handbrake (SDK patch 0133, src/input/wheel/wheel_handbrake.h, #303): where its axis
// is read (the wheel or a handbrake device), which device that is, and what the game gets (thumb_ly,
// the 360 wheel's analog handbrake). Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; g++ -std=c++17 -Wall -Wextra -Werror -I $SDK/include
//       -I $SDK/src/input/wheel forza-recomp/tests/wheel_handbrake_test.cpp -o wheel-handbrake-test
//   ./wheel-handbrake-test
#include <wheel_handbrake.h>

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

JoyInfo Joy(const char* name, uint16_t vid, uint16_t pid, bool sdl_wheel = false) {
  JoyInfo j;
  j.name = name;
  j.guid = "03000000000000000000000000000000";
  j.vid = vid;
  j.pid = pid;
  j.sdl_wheel = sdl_wheel;
  return j;
}
}  // namespace

int main() {
  // Where the axis is read: wheel_axes' handbrake first, then a connected handbrake device.
  HandbrakePlan p = PlanHandbrake({}, false, 0);
  Check(p.source == HandbrakeSource::kNone, "nothing set, no device: no handbrake axis");
  p = PlanHandbrake({}, true, 0);
  Check(p.source == HandbrakeSource::kDevice && p.axis == 0, "a handbrake device: its axis");
  p = PlanHandbrake({{"throttle", 2}}, true, 3);
  Check(p.source == HandbrakeSource::kDevice && p.axis == 3, "wheel_handbrake_axis");
  p = PlanHandbrake({{"handbrake", 4}}, true, 0);
  Check(p.source == HandbrakeSource::kWheel && p.axis == 4, "wheel_axes handbrake=4 wins");
  p = PlanHandbrake({{"handbrake", 99}}, true, 0);
  Check(p.source == HandbrakeSource::kWheel && p.axis == 99,
        "the launcher's 'not bound' (99) reads nothing, not the device either");
  p = PlanHandbrake({{"handbrake", -1}}, false, 0);
  Check(p.source == HandbrakeSource::kNone, "a negative number is unset");

  // Which device: a known handbrake or one named like one; a named one; none.
  const std::vector<JoyInfo> joys = {
      Joy("SIMAGIC Alpha EVO Pro", 0x3670, 0x0501, true), Joy("SIMAGIC P1000", 0x0483, 0x0525),
      Joy("SIMAGIC TB-RS Handbrake", 0x3670, 0x0A04)};
  Check(PickHandbrakeDevice(joys, "") == 2, "a known handbrake by its id");
  Check(PickHandbrakeDevice(joys, "none") == -1, "none: no handbrake device");
  Check(PickHandbrakeDevice(joys, "p1000") == 1, "a named device: the pedal set's handbrake port");
  Check(PickHandbrakeDevice(joys, "0483:0525") == 1, "by vid:pid");
  Check(PickHandbrakeDevice(joys, "missing") == -1, "a named device that is not there");
  Check(PickHandbrakeDevice({Joy("Sim Hand Brake", 0x1234, 0x5678), Joy("USB Joystick", 1, 2)}, "") == 0,
        "named like a handbrake");
  Check(PickHandbrakeDevice({Joy("Logitech RS Shifter and Handbrake", 0x046D, 0xC278)}, "") == -1,
        "a combined shifter is the shifter device unless named");
  Check(PickHandbrakeDevice({Joy("Logitech RS Shifter and Handbrake", 0x046D, 0xC278)},
                            "rs shifter") == 0,
        "named, it is the handbrake device too");
  Check(PickHandbrakeDevice({Joy("Fanatec ClubSport Handbrake", 0x0EB7, 0x1A93),
                             Joy("Thrustmaster TSS", 0x044F, 0xB695)},
                            "") == 0,
        "the first of two");

  // The handbrake's axis is no pedal: pedal detection sees it centred.
  const std::vector<int16_t> rest = {0, -32768, -32768, -32768, -32768};
  int pedal[3] = {-1, -1, -1};
  const bool want[3] = {true, true, true};
  DetectPedalAxes(pedal, want, 0, RestWithout(rest, 2), false);
  Check(pedal[0] == 1 && pedal[1] == 3 && pedal[2] == 4, "the handbrake axis is skipped");
  Check(RestWithout(rest, 9) == rest && RestWithout(rest, -1) == rest, "out of range: unchanged");

  // The game: thumb_ly up, released 0, full 32767.
  Check(HandbrakeThumb(0.0f) == 0 && HandbrakeThumb(1.0f) == 32767, "released and full");
  Check(HandbrakeThumb(0.5f) == 16384 && HandbrakeThumb(-1.0f) == 0 && HandbrakeThumb(2.0f) == 32767,
        "half way, clamped");
  std::puts("wheel handbrake: ok");
  return 0;
}
