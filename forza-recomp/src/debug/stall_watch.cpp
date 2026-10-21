// forza - log long frames, with where the game's threads were during them (#187).
//
// Players see a hitch of about a second every half hour and the log shows nothing. A
// watchdog thread (always on) checks every 50 ms when the render thread last handed a
// frame to VdSwap. When no frame came for 200 ms, it logs one line and the guest call
// chains of the main loop's thread and the render thread (DescribeGuestThread,
// src/debug/fault_guard.cpp); when the frame comes, the VdSwap hook logs how long it took:
//
//   FORZA_STALL: no frame for 249 ms at t=90.2s (main loop 1 frames since the last one)
//   FORZA_STALL:   main loop thread F800007C start 825CEC88 lr 8241DBC8 chain 825CECC8 82B3A4FC
//   FORZA_STALL:   render thread F80011AC start 8261E318 lr 82616540 chain 8261E1C0 82B3A4FC
//   FORZA_STALL: frame took 534 ms from t=90.0s (main loop 2 frames meanwhile, chains above)
//
// The chains are read from the threads' PPCContexts and guest stacks without stopping
// them, as LogGuestThreads does when the window closes: a running thread's chain can be
// cut short or a frame behind, but nothing is suspended or signalled, so the watchdog
// cannot deadlock the game or stop it in a lock. Only these two threads, which live for
// the whole session: a stack of a thread that exits is released, and reading it could
// fault. The host side is not sampled: another thread's host stack can only be taken by
// interrupting it with a signal.
//
// Nothing is logged in the first 60 s after the first frame (boot and the first loads).
// At most one chain dump every 10 s and 50 a session, at most 100 "frame took" lines a
// session. No frame for 10 s logs "not presenting" once (a long load, or the window not
// drawing) and the end of it "presenting again". Long loads in the session log lines too.
//
// FORZA_STALL_TEST=1 makes the main loop sleep 500 ms once, 90 s after the first frame,
// to check the lines appear.

#include "stall_watch.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace forza {
std::string DescribeGuestThread(rex::system::XThread& thread);  // src/debug/fault_guard.cpp
}

namespace forza::stall {

namespace {

constexpr int64_t kMs = 1000000;              // steady clock ns
constexpr int64_t kStall = 200 * kMs;         // a frame this long is a stall
constexpr int64_t kNotPresenting = 10000 * kMs;
constexpr int64_t kGrace = 60000 * kMs;       // after the first frame
constexpr int64_t kDumpInterval = 10000 * kMs;
constexpr int kMaxDumps = 50;
constexpr int kMaxFrameLines = 100;

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<int64_t> g_first_swap{0}, g_last_swap{0};
std::atomic<uint64_t> g_main_frames{0}, g_main_frames_at_swap{0};
std::atomic<uint32_t> g_render_thread{0}, g_main_thread{0};  // guest handles
// The g_last_swap the watchdog dumped the chains for, and the one it logged
// "not presenting" for: the VdSwap hook that ends that gap says so.
std::atomic<int64_t> g_dumped_gap{0}, g_absent_gap{0};

double Seconds(int64_t t) { return double(t - g_first_swap.load()) / 1e9; }

uint32_t CurrentGuestThread() {
  return rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThreadHandle()
                                            : 0;
}

void LogThread(const char* name, uint32_t handle) {
  auto* kernel = rex::system::kernel_state();
  if (!handle || !kernel || !kernel->object_table()) return;
  auto thread = kernel->object_table()->LookupObject<rex::system::XThread>(handle);
  if (!thread) return;
  const std::string line = DescribeGuestThread(*thread);
  if (!line.empty()) REXLOG_WARN("FORZA_STALL:   {} {}", name, line);
}

void Watch() {
  int dumps = 0;
  int64_t last_dump = 0, seen_gap = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t first = g_first_swap.load(), last = g_last_swap.load();
    const int64_t now = Now();
    if (!first || !last || now - first < kGrace || now - last < kStall) continue;
    if (now - last >= kNotPresenting && g_absent_gap.load() != last) {
      g_absent_gap = last;
      REXLOG_WARN("FORZA_STALL: no frame for {:.0f} s at t={:.1f}s, not presenting",
                  double(now - last) / 1e9, Seconds(now));
    }
    if (seen_gap == last) continue;  // this gap is handled
    seen_gap = last;
    if (dumps >= kMaxDumps || (last_dump && now - last_dump < kDumpInterval)) continue;
    ++dumps;
    last_dump = now;
    g_dumped_gap = last;
    REXLOG_WARN("FORZA_STALL: no frame for {} ms at t={:.1f}s (main loop {} frames since the "
                "last one){}",
                (now - last) / kMs, Seconds(now),
                g_main_frames.load() - g_main_frames_at_swap.load(),
                dumps == kMaxDumps ? "; the last call chains this session" : "");
    LogThread("main loop", g_main_thread.load());
    LogThread("render", g_render_thread.load());
  }
}

}  // namespace

void Start() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(Watch).detach(); });
}

void OnGuestSwap() {
  const int64_t now = Now();
  if (!g_render_thread.load(std::memory_order_relaxed)) g_render_thread = CurrentGuestThread();
  const uint64_t main_frames = g_main_frames.load(std::memory_order_relaxed);
  const uint64_t main_before = g_main_frames_at_swap.exchange(main_frames);
  const int64_t prev = g_last_swap.exchange(now);
  if (!prev) {
    g_first_swap = now;
    return;
  }
  if (now - prev < kStall || now - g_first_swap.load() < kGrace) return;
  if (g_absent_gap.load() == prev) {
    REXLOG_WARN("FORZA_STALL: presenting again after {:.1f} s at t={:.1f}s",
                double(now - prev) / 1e9, Seconds(now));
    return;
  }
  static int lines = 0;
  if (lines > kMaxFrameLines) return;
  if (++lines > kMaxFrameLines) {
    REXLOG_WARN("FORZA_STALL: more long frames this session are not logged");
    return;
  }
  REXLOG_WARN("FORZA_STALL: frame took {} ms from t={:.1f}s (main loop {} frames meanwhile{})",
              (now - prev) / kMs, Seconds(prev), main_frames - main_before,
              g_dumped_gap.load() == prev ? ", chains above" : "");
}

void OnMainFrame() {
  if (!g_main_thread.load(std::memory_order_relaxed)) g_main_thread = CurrentGuestThread();
  g_main_frames.fetch_add(1, std::memory_order_relaxed);
  static const bool test = [] {
    const char* v = std::getenv("FORZA_STALL_TEST");
    return v && *v && *v != '0';
  }();
  static bool tested = false;
  const int64_t first = g_first_swap.load(std::memory_order_relaxed);
  if (test && !tested && first && Now() - first > 90000 * kMs) {
    tested = true;
    REXLOG_INFO("FORZA_STALL_TEST: main loop sleeps 500 ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

}  // namespace forza::stall
