// The racing wheel's game force feedback (SDK patch 0101, rex/input/wheel/wheel_ff.h): the effects
// Forza Horizon sends through XInputdFF*, their state and what they ask of the wheel. Build
// against the patched SDK source (tools/build_rexglue_sdk.sh --source-only), from the
// repository root:
//   g++ -std=c++17 -Wall -Wextra -Werror -I ~/rexglue-sdk-src-forza/sdk/include
//       forza-recomp/tests/wheel_ff_test.cpp -o wheel-ff-test && ./wheel-ff-test
#include <rex/input/wheel/wheel_ff.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace rex::input::wheel;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// An effect as FH1 lays it out (sub_82CB76B8): 29 big-endian words.
struct Guest {
  uint8_t bytes[kFfEffectWords * 4] = {};
  void Set(size_t word, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    bytes[4 * word] = uint8_t(u >> 24);
    bytes[4 * word + 1] = uint8_t(u >> 16);
    bytes[4 * word + 2] = uint8_t(u >> 8);
    bytes[4 * word + 3] = uint8_t(u);
  }
};

// FH1's effect blocks with ControllerFFB.ini and a force feedback scale of 1.
Guest Fh1Effect(uint32_t block) {
  static const uint32_t kTypes[] = {10, 11, 9, 8, 1};  // inertia, friction, damper, spring, constant
  Guest g;
  g.Set(0, int32_t(block));
  g.Set(1, int32_t(kTypes[block - 1]));
  g.Set(2, 4);
  g.Set(3, 0xFF);
  g.Set(4, 0xFF0);
  g.Set(7, 0xFF);
  g.Set(8, 0xFF);
  if (block == 3) {  // damper: DamperScale 0.1, DamperMaxForce 0.1
    g.Set(12, 12);
    g.Set(13, 12);
    g.Set(14, 25);
    g.Set(15, 25);
  } else if (block == 4) {  // spring: SpringScale 0.5, SpringMaxForce 0.5
    g.Set(12, 63);
    g.Set(13, 63);
    g.Set(14, 127);
    g.Set(15, 127);
  }
  return g;
}
}  // namespace

int main() {
  // Decoding: big-endian words, the type's parameters from word 11, negative values kept.
  Guest c = Fh1Effect(5);
  c.Set(11, -64);
  const FfEffect constant = DecodeFfEffect(c.bytes);
  Check(constant.block == 5 && constant.type == kFfConstant && constant.gain == 255,
        "constant: block, type, gain");
  Check(constant.magnitude == -64, "constant: a negative magnitude");
  const FfEffect spring = DecodeFfEffect(Fh1Effect(4).bytes);
  Check(spring.type == kFfSpring && FfIsCondition(spring.type), "spring is a condition");
  Check(spring.center == 0 && spring.pos_coeff == 63 && spring.neg_coeff == 63 &&
            spring.pos_sat == 127 && spring.neg_sat == 127 && spring.deadband == 0,
        "spring: offset, coefficients, saturations, dead band");
  Check(!FfIsCondition(kFfConstant), "constant is not a condition");

  // Scaling: the effect's gain, the device gain and the player's strength.
  Check(Near(FfScale(constant, 255, 1.0f), 1.0f), "full scale");
  Check(Near(FfScale(constant, 255, 0.6f), 0.6f), "strength");
  Guest half = Fh1Effect(4);
  half.Set(3, 51);  // FH1's spring gain at low speed: 51/255
  Check(Near(FfScale(DecodeFfEffect(half.bytes), 255, 1.0f), 0.2f), "effect gain");
  Check(Near(FfScale(constant, 0, 1.0f), 0.0f), "device gain 0");
  Check(Near(FfScale(constant, 1000, 2.0f), 1.0f), "scale is clamped");

  // Constant force: -127..127 to -1..1, scaled and clamped.
  Check(Near(FfConstantLevel(constant, 1.0f), -64 / 127.0f), "constant level");
  c.Set(11, 300);
  Check(Near(FfConstantLevel(DecodeFfEffect(c.bytes), 0.5f), 0.5f), "constant level clamped");

  // Conditions: coefficients and saturations scaled together.
  const FfCondition sc = FfConditionOf(spring, 0.6f);
  Check(Near(sc.right_coeff, 63 / 127.0f * 0.6f) && Near(sc.left_coeff, sc.right_coeff),
        "spring coefficients");
  Check(Near(sc.right_sat, 127 / 255.0f * 0.6f) && Near(sc.left_sat, sc.right_sat),
        "spring saturations");
  Check(Near(sc.deadband, 0) && Near(sc.center, 0), "spring dead band and centre");
  Check(FfConditionFelt(sc), "the spring pushes");
  Check(!FfConditionFelt(FfConditionOf(DecodeFfEffect(Fh1Effect(1).bytes), 1.0f)),
        "FH1's inertia (all zero) pushes nothing");
  Check(!FfConditionFelt(FfConditionOf(spring, 0.0f)), "a spring at gain 0 pushes nothing");
  // Self-centring for a wheel without a spring effect: the weaker side, coefficient or saturation.
  Check(Near(FfSpringAutocenter(sc), 63 / 127.0f * 0.6f), "spring as self-centring");
  Guest lop = Fh1Effect(4);
  lop.Set(13, -20);
  Check(Near(FfSpringAutocenter(FfConditionOf(DecodeFfEffect(lop.bytes), 1.0f)), 0.0f),
        "a side that pushes away gives no self-centring");

  // The effect state, as FH1 drives it (sub_82CBB2D8): stop all, define 1-5, start them with 2.
  FfState s;
  s.Control(kFfStopAll);
  for (uint32_t b = 1; b <= 5; b++) {
    Check(s.Define(DecodeFfEffect(Fh1Effect(b).bytes)), "define blocks 1-5");
    Check(!s.Active(b), "a defined block waits for its start");
  }
  for (uint32_t b = 1; b <= 5; b++) {
    Check(s.Operate(b, 2), "start with 2");
    Check(s.Active(b), "started");
  }
  Check(!s.Operate(6, 2), "an undefined block cannot start");
  Check(!s.Operate(0, 2) && !s.Operate(kFfMaxBlocks + 1, 2), "blocks out of range");
  FfEffect bad = constant;
  bad.block = 0;
  Check(!s.Define(bad), "block 0 is rejected");
  bad.block = kFfMaxBlocks + 1;
  Check(!s.Define(bad), "a block past the last is rejected");
  // UpdateEffect replaces the definition and keeps it playing.
  Guest upd = Fh1Effect(5);
  upd.Set(11, 100);
  Check(s.Define(DecodeFfEffect(upd.bytes)) && s.effect(5).magnitude == 100 && s.Active(5),
        "update keeps the block playing");
  Check(s.Operate(5, kFfOpStop) && !s.Active(5) && s.Active(4), "stop one block");
  s.Operate(5, 1);
  Check(s.Active(5), "1 starts too");
  s.Control(kFfPause);
  Check(!s.Active(4), "paused");
  s.Control(kFfContinue);
  Check(s.Active(4), "continued");
  s.Control(kFfDisableActuators);
  Check(!s.Active(4), "actuators off");
  s.Control(kFfEnableActuators);
  Check(s.Active(4), "actuators on");
  s.Control(kFfStopAll);
  Check(!s.Active(4) && s.defined(4), "stop all keeps the blocks");
  s.Control(kFfReset);
  Check(!s.defined(4) && !s.defined(5), "reset frees the blocks");
  // Self-centring for a wheel without a spring effect follows the whole state (#219 review): a
  // spring that stops, is reset or becomes another type must take it back to 0.
  FfState a;
  const float spring_level = FfSpringAutocenter(FfConditionOf(spring, 0.6f));
  Check(Near(FfAutocenter(a, 0.6f), 0.0f), "autocentre: nothing defined");
  a.Define(spring);
  Check(Near(FfAutocenter(a, 0.6f), 0.0f), "autocentre: a defined spring that waits for its start");
  a.Operate(4, 2);
  Check(spring_level > 0.0f && Near(FfAutocenter(a, 0.6f), spring_level), "autocentre: running spring");
  a.Control(kFfStopAll);
  Check(Near(FfAutocenter(a, 0.6f), 0.0f), "autocentre: stop all");
  a.Operate(4, 2);
  Check(Near(FfAutocenter(a, 0.6f), spring_level), "autocentre: restarted");
  a.Control(kFfReset);
  Check(!a.defined(4) && Near(FfAutocenter(a, 0.6f), 0.0f), "autocentre: reset");
  a.Define(spring);
  a.Operate(4, 2);
  a.Control(kFfPause);
  Check(Near(FfAutocenter(a, 0.6f), 0.0f), "autocentre: paused");
  a.Control(kFfContinue);
  Check(Near(FfAutocenter(a, 0.6f), spring_level), "autocentre: continued");
  FfEffect as_damper = spring;
  as_damper.type = kFfDamper;
  a.Define(as_damper);
  Check(Near(FfAutocenter(a, 0.6f), 0.0f), "autocentre: the block redefined as a damper");
  a.Define(spring);
  Guest weak = Fh1Effect(4);
  weak.Set(0, 6);
  weak.Set(3, 51);
  a.Define(DecodeFfEffect(weak.bytes));
  a.Operate(6, 2);
  Check(Near(FfAutocenter(a, 0.6f), spring_level), "autocentre: the strongest spring wins");
  a.Operate(4, kFfOpStop);
  Check(Near(FfAutocenter(a, 0.6f), spring_level * 0.2f), "autocentre: the other spring remains");
  s.SetDeviceGain(400);
  Check(s.device_gain() == 255, "device gain clamped");
  s.SetDeviceGain(128);
  Check(s.device_gain() == 128, "device gain");
  std::puts("wheel ff: all checks passed");
  return 0;
}
