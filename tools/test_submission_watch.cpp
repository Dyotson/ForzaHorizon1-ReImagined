// How many draws one Vulkan submission holds on integrated GPUs, and which slow waits for the
// GPU are logged (patch 0230, #268): src/graphics/vulkan/fh1_submission_watch.h of the patched
// SDK source. Build with the patched SDK source's src/graphics/vulkan on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/graphics/vulkan
//       tools/test_submission_watch.cpp -o test_submission_watch
#include "fh1_submission_watch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace w = rex::graphics::vulkan::fh1_submission_watch;

#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      std::fprintf(stderr, "submission watch: FAIL: %s\n", #x);    \
      std::exit(1);                                                 \
    }                                                               \
  } while (false)

int main() {
  // The limit: automatic (1000 on integrated GPUs, none elsewhere), off, or a number.
  CHECK(w::DrawLimit(-1, true) == 1000);
  CHECK(w::DrawLimit(-1, false) == 0);
  CHECK(w::DrawLimit(0, true) == 0);
  CHECK(w::DrawLimit(250, false) == 250);
  CHECK(!w::ShouldSplit(999, 1000) && w::ShouldSplit(1000, 1000) && w::ShouldSplit(5000, 1000));
  CHECK(!w::ShouldSplit(100000, 0));
  // A 5500-draw frame (a race on the APU) reaches the GPU in six parts.
  {
    uint32_t in_submission = 0, submissions = 1;
    for (int draw = 0; draw < 5500; ++draw) {
      if (w::ShouldSplit(in_submission, w::DrawLimit(-1, true))) {
        ++submissions;
        in_submission = 0;
      }
      ++in_submission;
    }
    CHECK(submissions == 6 && in_submission == 500);
  }
  // Records of the last 64 submissions.
  {
    w::Watch watch;
    CHECK(!watch.Find(1) && !watch.Find(0));
    for (uint64_t s = 1; s <= 100; ++s) watch.Submitted(s, uint32_t(s * 10), int64_t(s));
    CHECK(watch.Find(100) && watch.Find(100)->draws == 1000 && watch.Find(100)->submitted_ms == 100);
    CHECK(watch.Find(37) && watch.Find(37)->draws == 370);
    CHECK(!watch.Find(36));  // overwritten by 100
  }
  // Slow waits: 250 ms or more, logged at most every 10 s and 50 times; the count covers the
  // slow waits between the logged ones.
  {
    w::Watch watch;
    CHECK(!watch.ShouldLogWait(249, 0));
    CHECK(watch.ShouldLogWait(250, 1000));
    CHECK(watch.TakeSlowWaits() == 1);
    CHECK(!watch.ShouldLogWait(400, 5000));
    CHECK(!watch.ShouldLogWait(300, 10999));
    CHECK(watch.ShouldLogWait(260, 11000));
    CHECK(watch.TakeSlowWaits() == 3);
    int64_t t = 11000;
    int logged = 2;
    for (int i = 0; i < 100; ++i) {
      t += 10000;
      logged += watch.ShouldLogWait(300, t);
    }
    CHECK(logged == 50);
  }
  std::puts("submission watch: ok");
  return 0;
}
