// When the Vulkan backend frees cached render targets (patch 0112, #241): the frame-end
// policy in src/graphics/vulkan/fh1_render_target_trim.h of the patched SDK source.
// Build with the patched SDK source's src/graphics/vulkan on the include path:
//   c++ -std=c++17 -I ~/rexglue-sdk-src-forza/sdk/src/graphics/vulkan
//       tools/test_render_target_trim.cpp -o test_render_target_trim
#include "fh1_render_target_trim.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace trim = rex::graphics::vulkan::fh1_render_target_trim;

#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      std::fprintf(stderr, "render target trim: FAIL: %s\n", #x);   \
      std::exit(1);                                                 \
    }                                                               \
  } while (false)

namespace {

constexpr uint64_t kMB = uint64_t(1) << 20;
constexpr uint64_t kGB = uint64_t(1) << 30;

// Runs frames [first, last) the way EndSubmission does; returns the frames that trimmed.
struct Session {
  uint64_t next_check = 0, next_allowed = 0;
  uint32_t percent = 90;
  bool have_budget = true;
  uint64_t budget = 16000 * kMB;
  int queries = 0, trims = 0;
  uint64_t last_trim = 0;
  void Run(uint64_t first, uint64_t last, uint64_t usage, bool failed_at_first = false,
           uint64_t freed = 4000 * kMB) {
    for (uint64_t frame = first; frame < last; ++frame) {
      bool failed = failed_at_first && frame == first;
      if (!trim::ShouldQuery(frame, next_check, next_allowed, percent, failed, have_budget))
        continue;
      ++queries;
      if (!failed && !trim::OverBudget(usage, have_budget ? budget : 0, percent)) continue;
      ++trims;
      last_trim = frame;
      next_allowed = trim::NextAllowedFrame(frame, failed, freed);
    }
  }
};

}  // namespace

int main() {
  // Below 90 % of the budget: queried every 30 frames, never trimmed.
  {
    Session s;
    s.Run(0, 3000, 14000 * kMB);
    CHECK(s.queries == 100);
    CHECK(s.trims == 0);
  }
  // At 90 % and above: one trim, then not again for 300 frames (5 s at 60 fps).
  {
    Session s;
    s.Run(0, 1, 14400 * kMB);
    CHECK(s.trims == 1 && s.last_trim == 0);
    s.Run(1, 300, 15900 * kMB);
    CHECK(s.trims == 1);
    s.Run(300, 301, 15900 * kMB);
    CHECK(s.trims == 2 && s.last_trim == 300);
  }
  // A failed allocation trims at the next frame end without a query of its own, then
  // again after 60 frames.
  {
    Session s;
    s.Run(0, 10, 1000 * kMB);  // frame 0 queried, below budget
    s.Run(17, 18, 1000 * kMB, true);
    CHECK(s.trims == 1 && s.last_trim == 17);
    s.Run(18, 77, 1000 * kMB, true);  // failure at frame 18: still waiting
    CHECK(s.trims == 1);
    s.Run(77, 78, 1000 * kMB, true);
    CHECK(s.trims == 2 && s.last_trim == 77);
  }
  // Little to free (the memory is full of other things): wait 1800 frames, not 300.
  {
    Session s;
    s.Run(0, 1, 15000 * kMB, false, 100 * kMB);
    s.Run(1, 1800, 15000 * kMB, false, 100 * kMB);
    CHECK(s.trims == 1);
    s.Run(1800, 1801, 15000 * kMB);
    CHECK(s.trims == 2);
  }
  // 0 switches it off, failures included; without VK_EXT_memory_budget only failures trim.
  {
    Session s;
    s.percent = 0;
    s.Run(0, 600, 15900 * kMB, true);
    CHECK(s.queries == 0 && s.trims == 0);
    Session n;
    n.have_budget = false;
    n.Run(0, 600, 15900 * kMB);
    CHECK(n.trims == 0);
    n.Run(600, 601, 15900 * kMB, true);
    CHECK(n.trims == 1);
  }
  CHECK(!trim::OverBudget(15000 * kMB, 0, 90));
  CHECK(trim::OverBudget(9 * kMB, 10 * kMB, 90));
  CHECK(!trim::OverBudget(8 * kMB, 10 * kMB, 90));
  // #268 (patch 0138): on integrated GPUs with a small carve-out, where new textures go to
  // system memory, a nearly full device-local heap does not trim; a device-local heap over its
  // whole budget, the system memory heap at the percentage, render targets in system memory
  // past their limit (512 MB or an eighth of the system budget) or a failure does. Elsewhere
  // (discrete GPUs, RADV's APUs) as before.
  {
    using R = trim::Reason;
    auto m = [](uint64_t usage, uint64_t budget, bool heap_over, uint64_t system_usage,
                uint64_t system_budget, uint64_t render_targets) {
      trim::Memory x;
      x.usage = usage, x.budget = budget, x.heap_over_budget = heap_over;
      x.system_usage = system_usage, x.system_budget = system_budget;
      x.render_targets_in_system = render_targets;
      return x;
    };
    const uint64_t b = 486 * kMB, sb = 7000 * kMB;
    CHECK(trim::ShouldTrim(true, false, m(470 * kMB, b, false, 2000 * kMB, sb, 300 * kMB), 90) ==
          R::kNone);
    CHECK(trim::ShouldTrim(false, false, m(470 * kMB, b, false, 2000 * kMB, sb, 0), 90) ==
          R::kMemoryFull);
    CHECK(trim::ShouldTrim(false, false, m(400 * kMB, b, true, 6900 * kMB, sb, 2 * kGB), 90) ==
          R::kNone);  // discrete: 0112's rule only
    // One device-local heap over its own budget, although the pooled use is 51 %.
    CHECK(trim::ShouldTrim(true, false, m(250 * kMB, b, true, 2000 * kMB, sb, 0), 90) ==
          R::kMemoryFull);
    CHECK(trim::ShouldTrim(true, false, m(300 * kMB, b, false, 6400 * kMB, sb, 0), 90) ==
          R::kMemoryFull);
    CHECK(trim::ShouldTrim(true, true, m(100 * kMB, b, false, 100 * kMB, sb, 0), 90) ==
          R::kAllocationFailed);
    // Render targets in system memory: the limit is an eighth of 7000 MB (875 MB) here...
    CHECK(trim::RenderTargetsInSystemLimit(sb) == 875 * kMB);
    CHECK(trim::ShouldTrim(true, false, m(470 * kMB, b, false, 2000 * kMB, sb, 874 * kMB), 90) ==
          R::kNone);
    CHECK(trim::ShouldTrim(true, false, m(470 * kMB, b, false, 2000 * kMB, sb, 875 * kMB), 90) ==
          R::kRenderTargetsInSystemMemory);
    // ...and at least 512 MB (an 8 GB laptop's 3.5 GB budget, or no budget known).
    CHECK(trim::RenderTargetsInSystemLimit(3584 * kMB) == 512 * kMB);
    CHECK(trim::RenderTargetsInSystemLimit(0) == 512 * kMB);
    CHECK(trim::ShouldTrim(true, false, m(400 * kMB, 0, false, 0, 0, 511 * kMB), 90) == R::kNone);
    CHECK(trim::ShouldTrim(true, false, m(400 * kMB, 0, false, 0, 0, 512 * kMB), 90) ==
          R::kRenderTargetsInSystemMemory);
  }
  std::puts("render target trim: ok");
  return 0;
}
