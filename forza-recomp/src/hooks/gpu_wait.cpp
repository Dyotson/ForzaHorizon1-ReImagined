// forza - the D3D library's waits for the GPU without a busy loop (#45, docs/perf.md).
//
// When the CPU waits for the GPU (for ring buffer space, for a fence: the render thread
// waits for the previous frames' swaps, so this is where the 30 fps cap is spent),
// sub_823E91F0 and six other D3D functions start a wait record with sub_829F03B0
// (device, reason, start timebase) and call sub_829F04A8 until the GPU's read pointer
// (the command processor's write-back, device +0x2B10) has passed the point they need.
// Each call is a few nanoseconds of polling (the console's db16cyc delays do nothing
// here) and a check for a GPU hang; it returns 0 when the wait must stop (the device is
// lost), else 1. In a race the render thread spent 93% of a core in that loop (its own
// work: 7%), taking a core from the command processor and the rest of the game wherever
// they share one (a Steam Deck: 4 cores, 8 threads, one power budget with the GPU).
//
// Now each wait keeps the game's loop for its first 20 us (short waits stay as quick),
// then gives the core away between polls: yield up to 200 us, then sleep 50 us per poll
// (Linux, with this thread's timer slack at 5 us; Windows keeps yielding: its sleeps are
// whole milliseconds). The command processor writes the read pointer only after each
// batch of commands it executes, so the waits that last are long ones (a vblank).
// FORZA_GPU_WAIT=spin keeps the game's loop; FORZA_GPU_WAIT_LOG=1 logs the waits every
// 10 s (count, total and longest time, polls).

#include "../platform/guest_addr.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/thread.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifndef _WIN32
#include <time.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kSpin = std::chrono::microseconds(20);
constexpr auto kYield = std::chrono::microseconds(200);
constexpr long kSleepNs = 50'000;

bool Spin() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_GPU_WAIT");
    return v && std::strcmp(v, "spin") == 0;
  }();
  return on;
}

bool Log() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_GPU_WAIT_LOG");
    return v && *v && *v != '0';
  }();
  return on;
}

struct WaitState {
  Clock::time_point start, last_poll;
  uint64_t polls = 0;
  bool slack_set = false;
  // FORZA_GPU_WAIT_LOG
  Clock::time_point window_start;
  uint64_t waits = 0, total_polls = 0, long_waits = 0;
  Clock::duration total{}, longest{};
};
thread_local WaitState t_wait;

void EndOfWait() {
  WaitState& w = t_wait;
  if (!Log() || w.polls == 0) return;
  const auto now = w.last_poll;
  const auto took = now - w.start;
  w.waits++;
  w.total_polls += w.polls;
  w.total += took;
  if (took > w.longest) w.longest = took;
  if (took > kYield) w.long_waits++;
  if (w.window_start == Clock::time_point{}) w.window_start = now;
  if (now - w.window_start >= std::chrono::seconds(10)) {
    using us = std::chrono::microseconds;
    REXLOG_INFO("GPUWAIT: waits={} long={} total_ms={:.1f} longest_ms={:.2f} polls={} mode={}",
                w.waits, w.long_waits, std::chrono::duration_cast<us>(w.total).count() / 1000.0,
                std::chrono::duration_cast<us>(w.longest).count() / 1000.0, w.total_polls,
                Spin() ? "spin" : "sleep");
    w.window_start = now;
    w.waits = w.total_polls = w.long_waits = 0;
    w.total = w.longest = {};
  }
}

void BetweenPolls(Clock::time_point now) {
  WaitState& w = t_wait;
  const auto waited = now - w.start;
  if (waited < kSpin) return;
  if (waited < kYield) {
    rex::thread::MaybeYield();
    return;
  }
#ifdef _WIN32
  rex::thread::MaybeYield();
#else
#ifdef __linux__
  if (!w.slack_set) {
    prctl(PR_SET_TIMERSLACK, 5000UL);
    w.slack_set = true;
  }
#endif
  timespec ts{0, kSleepNs};
  nanosleep(&ts, nullptr);
#endif
}

}  // namespace

// Start of a wait: (r3 = the wait record, r4 = device, r5 = reason).
extern "C" REX_FUNC(__imp__sub_829F03B0);
extern "C" REX_FUNC(sub_829F03B0) {
  EndOfWait();  // a wait that ended without its end call (the loop's early exits)
  t_wait.start = Clock::now();
  t_wait.polls = 0;
  __imp__sub_829F03B0(ctx, base);
}

// One poll of the wait: r3 = 1 while it may go on (the caller then checks the read
// pointer again), 0 to give up.
extern "C" REX_FUNC(__imp__sub_829F04A8);
extern "C" REX_FUNC(sub_829F04A8) {
  __imp__sub_829F04A8(ctx, base);
  WaitState& w = t_wait;
  w.polls++;
  if (Spin() && !Log()) return;
  w.last_poll = Clock::now();
  if (ctx.r3.u32 != 0 && !Spin()) BetweenPolls(w.last_poll);
}
