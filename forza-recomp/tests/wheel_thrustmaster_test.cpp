// Thrustmaster wheels by USB id and a wheel whose axes all read 0 when opened (SDK patch 0135,
// src/input/wheel/wheel_thrustmaster.h, #308). Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; g++ -std=c++17 -Wall -Wextra -Werror -I $SDK/include
//       -I $SDK/src/input/wheel forza-recomp/tests/wheel_thrustmaster_test.cpp -o wheel-thrustmaster-test
//   ./wheel-thrustmaster-test
#include <wheel_thrustmaster.h>

#include <cstdio>
#include <cstdlib>
#include <set>

using namespace rex::input::wheel;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}
}  // namespace

int main() {
  // The TMX of #308: 044f:b67f, X steering, Y brake, Rz throttle, Slider clutch: SDL axes 0 1 2 3.
  const ThrustmasterModel* tmx = FindThrustmaster(0x044F, 0xB67F);
  Check(tmx && tmx->axes == 4, "the TMX by id");
  Check(tmx->steer == 0 && tmx->brake == 1 && tmx->throttle == 2 && tmx->clutch == 3, "TMX axes");
  Check(ThrustmasterAxes(tmx, 4) == tmx && ThrustmasterAxes(tmx, 5) == nullptr &&
            ThrustmasterAxes(nullptr, 4) == nullptr,
        "its axes only with four axes");
  Check(!FindThrustmaster(0x046D, 0xB67F) && !FindThrustmaster(0x044F, 0x1234), "others: none");
  Check(!FindThrustmaster(0x044F, 0xB696), "the T248 is left out (no source for its axes)");
  // Every model: the paddles on 0 and 1 are LB and RB, axes and buttons distinct and in range.
  for (const auto& m : kThrustmasterModels) {
    Check(m.lb == 0 && m.rb == 1, "the paddles shift");
    const std::set<int> axes = {m.steer, m.throttle, m.brake, m.clutch};
    Check(axes.size() == 4 && *axes.rbegin() < m.axes && *axes.begin() >= 0, "four axes in range");
    std::set<int> buttons;
    int count = 0;
    for (int b : {m.a, m.b, m.x, m.y, m.lb, m.rb, m.back, m.start, m.ls, m.rs}) {
      if (b >= 0) {
        buttons.insert(b);
        count++;
      }
    }
    Check(static_cast<int>(buttons.size()) == count, "no button twice");
  }
  const ThrustmasterModel* tgt = FindThrustmaster(0x044F, 0xB684);
  Check(tgt && tgt->axes == 8 && tgt->throttle == 5 && tgt->clutch == 6, "T-GT: Rz 5, Slider 6");
  // Buttons: the model's, then wheel_buttons over them.
  auto b = ThrustmasterButtons(tmx, {});
  Check(b["a"] == 4 && b["b"] == 8 && b["x"] == 2 && b["y"] == 9 && b["lb"] == 0 && b["rb"] == 1 &&
            b["back"] == 6 && b["start"] == 7 && b["ls"] == 5 && b["rs"] == 10 && !b.count("up"),
        "TMX buttons, the hat stays the d-pad");
  b = ThrustmasterButtons(tmx, {{"a", 11}, {"up", 12}});
  Check(b["a"] == 11 && b["up"] == 12 && b["b"] == 8, "wheel_buttons wins");
  Check(ThrustmasterButtons(nullptr, {{"x", 3}}) == std::map<std::string, int>{{"x", 3}},
        "another wheel: wheel_buttons only");
  const ThrustmasterModel* t300 = FindThrustmaster(0x044F, 0xB66E);
  b = ThrustmasterButtons(t300, {});
  Check(b["a"] == 5 && b["b"] == 4 && b["x"] == 3 && b["y"] == 2, "T300: cross, circle, square, triangle");
  // Every axis at exactly 0: no report yet; the first one that is not 0 brings the rest.
  Check(RestPending({0, 0, 0, 0}), "all 0: pending");
  Check(!RestPending({0, 32767, 32767, 32767}) && !RestPending({0, 0, -1, 0}), "a real rest");
  Check(!RestPending({0}) && !RestPending({}), "one axis or none: nothing to wait for");
  Check(!RestArrived({0, 0, 0, 0}) && RestArrived({0, 32767, 0, 0}), "the first report");
  std::puts("wheel thrustmaster: ok");
  return 0;
}
