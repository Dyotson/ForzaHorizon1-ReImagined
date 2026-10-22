// Controller remapping (patch 0116, #279): the controller_remap rules in
// src/input/sdl/gamepad_remap.h of the patched SDK source (no SDL needed).
// Build with the patched SDK source's src/input/sdl on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/input/sdl
//       tools/test_gamepad_remap.cpp -o test_gamepad_remap
#include "gamepad_remap.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace remap = rex::input::sdl::remap;

#define CHECK(x)                                                \
  do {                                                          \
    if (!(x)) {                                                 \
      std::fprintf(stderr, "gamepad remap: FAIL line %d: %s\n", __LINE__, #x); \
      std::exit(1);                                             \
    }                                                           \
  } while (false)

namespace {

// SDL button and axis numbers (SDL_GamepadButton, SDL_GamepadAxis).
constexpr int kA = 0, kGuide = 5, kStart = 6, kRb = 10, kMisc1 = 15, kPaddle1 = 16, kPaddle4 = 19;
constexpr int kLx = 0, kLy = 1, kRx = 2, kRy = 3, kLt = 4, kRt = 5;
constexpr uint16_t A = 0x1000, Y = 0x8000, LB = 0x0100, RB = 0x0200, START = 0x0010, GUIDE = 0x0400;

remap::PadInput Pad(uint32_t buttons = 0) {
  remap::PadInput p;
  p.buttons = buttons;
  return p;
}
uint32_t Bit(int b) { return 1u << b; }

// The driver's mapping before remapping existed, for an input.
remap::PadOutput Old(const remap::PadInput& in, bool guide) {
  static const uint16_t lookup[21] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0020, 0x0400, 0x0010,
                                      0x0040, 0x0080, 0x0100, 0x0200, 0x0001, 0x0002, 0x0004,
                                      0x0008, 0x0400, 0x8000, 0x2000, 0x4000, 0x1000, 0x0400};
  remap::PadOutput o;
  for (int i = 0; i < 21; i++) {
    if (((in.buttons >> i) & 1) && (lookup[i] != GUIDE || guide)) o.buttons |= lookup[i];
  }
  o.thumb_lx = in.axis[kLx];
  o.thumb_ly = static_cast<int16_t>(~in.axis[kLy]);
  o.thumb_rx = in.axis[kRx];
  o.thumb_ry = static_cast<int16_t>(~in.axis[kRy]);
  o.left_trigger = static_cast<uint8_t>(in.axis[kLt] >> 7);
  o.right_trigger = static_cast<uint8_t>(in.axis[kRt] >> 7);
  return o;
}

bool Same(const remap::PadOutput& a, const remap::PadOutput& b) {
  return a.buttons == b.buttons && a.left_trigger == b.left_trigger &&
         a.right_trigger == b.right_trigger && a.thumb_lx == b.thumb_lx &&
         a.thumb_ly == b.thumb_ly && a.thumb_rx == b.thumb_rx && a.thumb_ry == b.thumb_ry;
}

void TestNoRulesIsTheOldMapping() {
  const remap::Rules none{};
  CHECK(!remap::HasRules(none));
  uint32_t seed = 12345;
  for (int n = 0; n < 20000; n++) {
    remap::PadInput in;
    seed = seed * 1103515245u + 12345u;
    in.buttons = seed & 0x1FFFFF;
    for (int a = 0; a < 6; a++) {
      seed = seed * 1103515245u + 12345u;
      in.axis[a] = static_cast<int16_t>(seed >> 8);
      if (a >= kLt && in.axis[a] < 0) in.axis[a] = static_cast<int16_t>(-(in.axis[a] + 1));
    }
    in.axis[kLy] = n == 0 ? -32768 : in.axis[kLy];
    for (bool guide : {false, true}) CHECK(Same(remap::Apply(none, in, guide), Old(in, guide)));
  }
}

void TestButtonToButtonAndTrigger() {
  // Throttle on A: a full press of RT. A keeps pressing A until it is cleared.
  auto r = remap::ParseRules("rt=a");
  auto out = remap::Apply(r, Pad(Bit(kA)), false);
  CHECK(out.right_trigger == 255 && (out.buttons & A));
  r = remap::ParseRules("rt=a, A = none ,lb=rb,rb=lb");
  out = remap::Apply(r, Pad(Bit(kA) | Bit(kRb)), false);
  CHECK(out.right_trigger == 255 && !(out.buttons & A) && (out.buttons & LB) && !(out.buttons & RB));
  // The physical RT no longer accelerates once RT is bound elsewhere.
  remap::PadInput in = Pad();
  in.axis[kRt] = 32767;
  CHECK(remap::Apply(r, in, false).right_trigger == 0);
  // Paddles keep their default face buttons unless the face button is remapped.
  CHECK(remap::Apply(r, Pad(Bit(kPaddle4)), false).buttons == 0);
  CHECK(remap::Apply(remap::ParseRules("rt=paddle1"), Pad(Bit(kPaddle1)), false).buttons == Y);
  CHECK(remap::Apply(remap::ParseRules("y=b,rt=paddle1"), Pad(Bit(kPaddle1)), false).buttons == 0);
}

void TestAxisToTriggerAndButton() {
  // Throttle on the right stick pushed up, brake on it pulled down (SDL: up is negative).
  auto r = remap::ParseRules("rt=ry+,lt=ry-,ry=none");
  remap::PadInput in = Pad();
  in.axis[kRy] = -32768;  // fully up
  auto out = remap::Apply(r, in, false);
  CHECK(out.right_trigger == 255 && out.left_trigger == 0 && out.thumb_ry == 0);
  in.axis[kRy] = 16384;  // half down
  out = remap::Apply(r, in, false);
  CHECK(out.right_trigger == 0 && out.left_trigger == 128);
  // Handbrake on LT past 30%, start on the left stick pushed right past half.
  r = remap::ParseRules("a=lt@0.3,start=lx+");
  in = Pad();
  in.axis[kLt] = 9000;
  CHECK(!(remap::Apply(r, in, false).buttons & A));
  in.axis[kLt] = 10000;
  CHECK(remap::Apply(r, in, false).buttons & A);
  in.axis[kLx] = 16383;
  CHECK(!(remap::Apply(r, in, false).buttons & START));
  in.axis[kLx] = 16384;
  CHECK(remap::Apply(r, in, false).buttons & START);
  in.axis[kLx] = -32768;  // the other way
  CHECK(!(remap::Apply(r, in, false).buttons & START));
  // A trigger to the other trigger.
  r = remap::ParseRules("rt=lt,lt=rt");
  in = Pad();
  in.axis[kLt] = 32767;
  out = remap::Apply(r, in, false);
  CHECK(out.right_trigger == 255 && out.left_trigger == 0);
}

void TestStickSwapAndInvert() {
  auto r = remap::ParseRules("lx=rx,ly=ry,rx=lx,ry=ly");
  remap::PadInput in = Pad();
  in.axis[kLx] = 1000;
  in.axis[kLy] = -32768;  // left stick up
  in.axis[kRx] = -2000;
  in.axis[kRy] = 3000;
  auto out = remap::Apply(r, in, false);
  CHECK(out.thumb_lx == -2000 && out.thumb_ly == ~int16_t(3000) && out.thumb_rx == 1000 &&
        out.thumb_ry == 32767);
  r = remap::ParseRules("lx=-lx");
  in.axis[kLx] = -32768;
  CHECK(remap::Apply(r, in, false).thumb_lx == 32767);
  in.axis[kLx] = 32767;
  CHECK(remap::Apply(r, in, false).thumb_lx == -32768);
}

void TestGuideAndRejectedPairs() {
  auto r = remap::ParseRules("guide=start,start=misc1");
  auto out = remap::Apply(r, Pad(Bit(kStart) | Bit(kMisc1)), false);
  CHECK(out.buttons == START);  // no guide without the pass-through
  CHECK(remap::Apply(r, Pad(Bit(kStart)), true).buttons == GUIDE);
  CHECK(remap::Apply(remap::Rules{}, Pad(Bit(kGuide)), false).buttons == 0);
  int ignored = 0;
  // A stick axis from a button, a trigger inverted, half of a trigger, an unknown target,
  // and garbage are ignored; the rest applies.
  r = remap::ParseRules("lx=a,rt=-lt,lt=lt+,zz=a,b=@,y=lx-@x,x=b,rb=-lx", &ignored);
  CHECK(ignored == 7);
  CHECK(remap::Describe(r) == "x=b");
  CHECK(remap::Describe(remap::ParseRules("a=rt@0.3,rt=ly+,ry=-ry,lb=none")) ==
        "a=rt@0.30,lb=none,rt=ly+,ry=-ry");
  CHECK(remap::ParseRules("a=rt@9").at(0).threshold == 0.95f);
}

void TestProfiles() {
  const std::string xbox = "030000005e0400008e02000014010000";
  const std::string xbox_bt = "050000005e0400008e02000001000000";  // same model, other bus
  const std::string ds = "030000004c050000e60c000011810000";
  const std::string setting = "*:rt=a;" + ds + ":a=b;" + xbox + ":lb=rb";
  CHECK(remap::FindProfile(setting, xbox)->rules == "lb=rb");
  CHECK(remap::FindProfile(setting, "030000005E0400008E02000014010000")->rules == "lb=rb");
  CHECK(remap::FindProfile(setting, xbox_bt)->key == xbox);
  CHECK(remap::FindProfile(setting, ds)->rules == "a=b");
  CHECK(remap::FindProfile(setting, "03000000ffff00000100000000000000")->key == "*");
  CHECK(!remap::FindProfile(ds + ":a=b", xbox));
  CHECK(!remap::FindProfile("", xbox));
  // Reset keeps an explicit empty profile to block both model and wildcard fallbacks.
  for (const auto& fallback : {xbox + ":rt=a", std::string("*:rt=a")}) {
    for (const auto& text : {fallback + ";" + xbox_bt + ":", xbox_bt + ":;" + fallback}) {
      const auto profile = remap::FindProfile(text, xbox_bt);
      CHECK(profile && profile->key == xbox_bt && profile->rules.empty());
      const auto rules = remap::ParseRules(profile->rules);
      CHECK(!remap::HasRules(rules));
      remap::PadInput in = Pad(Bit(kA));
      CHECK(remap::Apply(rules, in, false).right_trigger == 0);
      in.axis[kRt] = 32767;
      CHECK(remap::Apply(rules, in, false).right_trigger == 255);
    }
  }
  CHECK(remap::GuidModel(ds) == std::make_pair(uint16_t(0x054C), uint16_t(0x0CE6)));
  // A GUID made from the name (no vendor and product) matches only itself.
  const std::string named = "05000000476f6f676c65205374616469";
  CHECK(!remap::GuidModel(named));
  CHECK(remap::FindProfile(named + ":a=b", named)->rules == "a=b");
}

}  // namespace

int main() {
  TestNoRulesIsTheOldMapping();
  TestButtonToButtonAndTrigger();
  TestAxisToTriggerAndButton();
  TestStickSwapAndInvert();
  TestGuideAndRejectedPairs();
  TestProfiles();
  std::printf("gamepad remap: all tests passed\n");
  return 0;
}
