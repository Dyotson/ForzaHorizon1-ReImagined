// forza - the render thread no longer waits forever for a HUD command buffer (#512).
// tu_address_map: TU4 addresses
//
// Part of the HUD is recorded in fixed-function command buffers (FFCB) on a worker thread and
// drawn on the render thread through a 12-slot ring in the render object R: the worker's submit
// (sub_824198A8, base sub_823E97F8) sets the slot event the render thread's consume
// (sub_824A1E40, base sub_824726C8) waits for without a timeout. Only the render thread wakes the
// worker (its kick, sub_82D11BA0, sets the wake event *R+0x764 unless its flag is still set). When
// FFCB turns on with the HUD map at free roam and the worker is a frame behind, the first consume
// waits for a submit the worker makes only when woken again, which only the waiting render thread
// would do: both wait forever (seen with a 50-75 Hz guest vblank, more often with faster code).
// Now a consume with every submit so far consumed wakes the worker itself every 2 ms until the
// submit comes (at most 2 s), then waits as the game does. FORZA_FFCB_WAKE=0: the game's wait.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#if FORZA_TU4
#define FfcbSubmit sub_824198A8
#define OriginalFfcbSubmit __imp__sub_824198A8
#define FfcbConsume sub_824A1E40
#define OriginalFfcbConsume __imp__sub_824A1E40
#define SetEventHandle sub_824AA568
#else
#define FfcbSubmit sub_823E97F8
#define OriginalFfcbSubmit __imp__sub_823E97F8
#define FfcbConsume sub_824726C8
#define OriginalFfcbConsume __imp__sub_824726C8
#define SetEventHandle sub_8247AF78
#endif

namespace {

constexpr uint32_t kRecording = 0x460;   // R: a recording to submit (byte)
constexpr uint32_t kWorkerWake = 0x764;  // R: pointer to the worker's wake event handle

std::atomic<uint32_t> ring{0};       // the render object the counts below are for
std::atomic<uint64_t> submitted{0};  // submits that set a slot's event
std::atomic<uint64_t> consumed{0};   // consumes that returned
std::atomic<uint64_t> wakes{0};

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_FFCB_WAKE");
    return !(v && std::strcmp(v, "0") == 0);
  }();
  return on;
}

// The counts belong to one render object; a new one (the device made again) starts them over.
void Track(uint32_t r) {
  uint32_t seen = ring.load(std::memory_order_acquire);
  if (seen != r && ring.compare_exchange_strong(seen, r, std::memory_order_acq_rel)) {
    submitted.store(0, std::memory_order_release);
    consumed.store(0, std::memory_order_release);
  }
}

bool Outstanding() {
  return submitted.load(std::memory_order_acquire) > consumed.load(std::memory_order_acquire);
}

}  // namespace

extern "C" REX_FUNC(SetEventHandle);

extern "C" REX_FUNC(OriginalFfcbSubmit);
extern "C" REX_FUNC(FfcbSubmit) {
  const uint32_t r = ctx.r3.u32;
  const bool recording = *forza::GuestPtr(base, r + kRecording) != 0;
  OriginalFfcbSubmit(ctx, base);
  if (!recording) return;
  Track(r);
  submitted.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" REX_FUNC(OriginalFfcbConsume);
extern "C" REX_FUNC(FfcbConsume) {
  const uint32_t r = ctx.r3.u32;
  Track(r);
  const uint32_t wake = Load32(base, r + kWorkerWake);
  if (Enabled() && wake && !Outstanding()) {
    const auto start = std::chrono::steady_clock::now();
    auto last_wake = start - std::chrono::seconds(1);
    while (!Outstanding() && std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
      if (std::chrono::steady_clock::now() - last_wake >= std::chrono::milliseconds(2)) {
        PPCContext call = ctx;
        call.r1.u64 = ctx.r1.u32 - 0x400;
        call.r3.u64 = Load32(base, wake);  // the event's handle
        SetEventHandle(call, base);
        last_wake = std::chrono::steady_clock::now();
      }
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    // The first ten, then at most one line every 10 s with the count since the last one.
    const auto now = std::chrono::steady_clock::now();
    const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    const uint64_t n = wakes.fetch_add(1, std::memory_order_relaxed) + 1;
    static std::atomic<int64_t> next_summary{0};
    static std::atomic<uint64_t> summarized{10};
    const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    if (n <= 10) {
      REXLOG_INFO("FFCB: the HUD command buffer the render thread waits for was not submitted: woke "
                  "the worker, {} after {} us (#512, {} so far)",
                  Outstanding() ? "submitted" : "still none", us, n);
      if (n == 10) next_summary.store(now_s + 10, std::memory_order_relaxed);
    } else if (now_s >= next_summary.load(std::memory_order_relaxed)) {
      next_summary.store(now_s + 10, std::memory_order_relaxed);
      const uint64_t before = summarized.exchange(n, std::memory_order_relaxed);
      REXLOG_INFO("FFCB: woke the HUD command buffer worker {} more times (#512, {} so far)",
                  n - before, n);
    }
  }
  OriginalFfcbConsume(ctx, base);
  consumed.fetch_add(1, std::memory_order_acq_rel);
}
