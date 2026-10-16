// forza - a zip member read that can make no progress ends instead of looping (#86).
//
// IOSys::CCompressedCacheStream reads a zip member whose compressed bytes sit in memory (a
// block of the archive's cache, or a page list). Its Read (sub_82C78C88, TU4 sub_82D39EA0)
// loops "decode a step (sub_82C76508, TU4 sub_82D37380) while bytes are wanted and the
// step says there is more". The step copies (method 0), inflates (8) or runs
// XMemDecompressStream (21) from the input left (+0x50) into the caller's buffer and says
// "more" whenever the member's output (+0x24) has not reached its size (+0x2C). When the
// input runs out before the output is complete, every later step copies nothing, still
// says "more", and the Read never returns: the resource loader thread (the only one) hangs
// in it and nothing else streams in.
//
// The hook: a step that wanted bytes but neither consumed input (+0x4C) nor produced output
// is retried a few times (the game's own result), then reported as the end (0). The read
// then returns short, as for a damaged member, and the loader moves on (the reader above it
// may ask again a few times; each ask ends the same way). One log line per stream (the first
// 20 streams): method, sizes, positions, the cache block and its load state.

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

constexpr uint32_t kOutPos = 0x24, kMethod = 0x28, kOutSize = 0x2C, kInSize = 0x30;
constexpr uint32_t kBase = 0x34, kBlock = 0x38, kPages = 0x3C, kInPos = 0x4C, kInLeft = 0x50;
constexpr int kStallsBeforeEnd = 8;

thread_local uint32_t stall_stream = 0;
thread_local int stalls = 0;
std::atomic<int> reports{0};
std::atomic<uint32_t> last_reported{0};

// FORZA_ZIPREAD_TEST=N (a test): the N-th decode step that wants bytes finds its stream's input
// used up, as a member whose compressed bytes ran out would (the hang without this hook).
std::atomic<long> test_step{[] {
  const char* v = std::getenv("FORZA_ZIPREAD_TEST");
  return v ? std::atol(v) : 0L;
}()};

}  // namespace

extern "C" REX_FUNC(__imp__sub_82C76508);
extern "C" REX_FUNC(sub_82C76508) {
  const uint32_t stream = ctx.r3.u32;
  const uint32_t wanted_ptr = ctx.r5.u32;
  const uint8_t* s = forza::GuestPtr(base, stream);
  const uint32_t out0 = Load32(s + kOutPos), in0 = Load32(s + kInPos);
  const uint32_t wanted = Load32(forza::GuestPtr(base, wanted_ptr));
  if (wanted && test_step.load(std::memory_order_relaxed) > 0 && test_step.fetch_sub(1) == 1) {
    REXLOG_WARN("ZIPREAD: test: stream {:08X} loses its input", stream);
    std::memset(forza::GuestPtr(base, stream + kInLeft), 0, 4);
  }
  __imp__sub_82C76508(ctx, base);
  if (!(ctx.r3.u32 & 0xFF) || !wanted || Load32(s + kOutPos) != out0 || Load32(s + kInPos) != in0) {
    if (stall_stream == stream) stalls = 0;
    return;
  }
  if (stall_stream != stream) {
    stall_stream = stream;
    stalls = 0;
  }
  if (++stalls < kStallsBeforeEnd) return;
  stalls = 0;
  ctx.r3.u64 = 0;
  if (last_reported.exchange(stream) != stream && reports.fetch_add(1) < 20) {
    // The cache block the compressed bytes are in (+0x38; its load state at +0x28/+0x29,
    // 4/4 = loaded, 6 = failed), or the page list (+0x3C).
    const uint32_t block = Load32(s + kBlock);
    const uint8_t* b = block ? forza::GuestPtr(base, block) : nullptr;
    REXLOG_ERROR("ZIPREAD: stream {:08X} (method {}, {} of {} bytes out, input {} of {} read, "
                 "{} left, block {:08X} state {}/{} at +{:X}, pages {:08X}) cannot go on; the "
                 "read ends short (#86)",
                 stream, Load16(s + kMethod), Load32(s + kOutPos), Load32(s + kOutSize),
                 Load32(s + kInPos), Load32(s + kInSize), Load32(s + kInLeft), block,
                 b ? b[0x28] : 0, b ? b[0x29] : 0, Load32(s + kBase), Load32(s + kPages));
  }
}
