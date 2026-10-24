// Which resolves "readback_resolve = some" does not copy back again after a gap (#45, patch
// 0181): src/graphics/vulkan/fh1_readback_history.h of the patched SDK source.
// Build with the patched SDK source's src/graphics/vulkan on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/graphics/vulkan
//       tools/test_readback_history.cpp -o test_readback_history
#include "fh1_readback_history.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace rh = rex::graphics::vulkan::fh1_readback_history;

#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      std::fprintf(stderr, "readback history: FAIL: %s\n", #x);     \
      std::exit(1);                                                 \
    }                                                               \
  } while (false)

int main() {
  constexpr uint64_t kScene = 0x1000, kDepth = 0x2000, kPicture = 0x3000, kPhoto = 0x4000;
  rh::History h;
  // Nothing is hot before it was seen.
  CHECK(!h.IsHot(kScene));
  // The scene's targets, resolved every frame (the depth target several times a frame).
  for (uint64_t f = 1; f < rh::kHotRunFrames; ++f) {
    h.Resolved(kScene, f);
    h.Resolved(kDepth, f);
    h.Resolved(kDepth, f);
  }
  CHECK(!h.IsHot(kScene) && !h.IsHot(kDepth));
  h.Resolved(kScene, rh::kHotRunFrames);
  h.Resolved(kDepth, rh::kHotRunFrames);
  CHECK(h.IsHot(kScene) && h.IsHot(kDepth));
  // A car picture now and then: a few frames each time, never a long run.
  uint64_t f = 1000;
  for (int picture = 0; picture < 200; ++picture) {
    for (int i = 0; i < 3; ++i) h.Resolved(kPicture, f + i);
    f += 90;
  }
  CHECK(!h.IsHot(kPicture));
  // A photo: once.
  h.Resolved(kPhoto, f);
  CHECK(!h.IsHot(kPhoto));
  // A gap (the map) does not make the scene's targets cold; a short run after it neither
  // lowers their longest run.
  h.Resolved(kScene, f + 1200);
  h.Resolved(kScene, f + 1201);
  CHECK(h.IsHot(kScene));
  // Every second frame still counts as a run (a target resolved at half the frame rate).
  rh::History half;
  for (uint64_t g = 0; g < 2 * rh::kHotRunFrames; g += 2) half.Resolved(7, g);
  CHECK(half.IsHot(7));
  // Every third frame does not.
  rh::History third;
  for (uint64_t g = 0; g < 3 * rh::kHotRunFrames * 3; g += 3) third.Resolved(8, g);
  CHECK(!third.IsHot(8));
  // Forgotten after kForgetFrames without a resolve.
  h.Forget(f + 1201 + rh::kForgetFrames);
  CHECK(h.IsHot(kScene));
  h.Forget(f + 1202 + rh::kForgetFrames);
  CHECK(!h.IsHot(kScene));
  // At most kMaxKeys keys: the oldest go first.
  rh::History many;
  for (uint64_t k = 0; k < rh::kMaxKeys + 100; ++k) many.Resolved(100000 + k, 10 + k);
  for (uint64_t g = 1; g <= rh::kHotRunFrames; ++g) many.Resolved(99, 20000 + g);
  many.Forget(20000 + rh::kHotRunFrames);
  CHECK(many.size() == rh::kMaxKeys);
  CHECK(many.IsHot(99));
  std::printf("readback history: all checks passed\n");
  return 0;
}
