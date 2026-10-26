// The game's force feedback effects a wheel refuses (SDK patch 0136, src/input/wheel/
// wheel_ff_reject.h, #308): the directions tried, a refused type not tried again until the game
// resets its force feedback. Build against the patched SDK source
// (tools/build_rexglue_sdk.sh --source-only), from the repository root:
//   SDK=~/rexglue-sdk-src-forza/sdk; g++ -std=c++17 -Wall -Wextra -Werror -I $SDK/include
//       -I $SDK/src/input/wheel forza-recomp/tests/wheel_ff_reject_test.cpp -o wheel-ff-reject-test
//   ./wheel-ff-reject-test
#include <wheel_ff_reject.h>

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
constexpr uint32_t kConstant = 1u << 0, kSpring = 1u << 7, kDamper = 1u << 8;  // SDL's type bits
}  // namespace

int main() {
  // Directions: the steering axis first, then Cartesian X, then polar; the one that worked first.
  Check((FfDirectionOrder(0) == std::array<int, 3>{0, 1, 2}), "steering axis first");
  Check((FfDirectionOrder(1) == std::array<int, 3>{1, 0, 2}), "the one that worked first");
  Check((FfDirectionOrder(2) == std::array<int, 3>{2, 0, 1}), "polar first");
  Check((FfDirectionOrder(7) == std::array<int, 3>{0, 1, 2}), "out of range: from the start");
  // A refused type: logged the first time only, not tried again; others still are.
  FfRejections r;
  Check(!r.Rejected(kConstant), "nothing refused yet");
  Check(r.Reject(kConstant), "refused: log it");
  Check(!r.Reject(kConstant), "the second time: not again");
  Check(r.Rejected(kConstant) && !r.Rejected(kSpring), "only that type");
  Check(r.Reject(kSpring) && r.types() == (kConstant | kSpring), "another type");
  r.Reset();
  Check(!r.Rejected(kConstant) && !r.Rejected(kSpring) && r.Reject(kDamper), "after a reset: again");
  // A reset of the game's force feedback: no block defined.
  FfState s;
  Check(FfNothingDefined(s), "before any effect");
  FfEffect e;
  e.block = 5;
  e.type = kFfConstant;
  s.Define(e);
  Check(!FfNothingDefined(s), "an effect defined");
  s.Control(kFfStopAll);
  Check(!FfNothingDefined(s), "stop all keeps the definitions");
  s.Control(kFfReset);
  Check(FfNothingDefined(s), "a reset clears them");
  std::puts("wheel ff reject: ok");
  return 0;
}
