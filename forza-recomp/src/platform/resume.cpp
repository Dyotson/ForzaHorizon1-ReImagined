// forza - a game that was stopped or whose machine slept (#140, docs/playing.md, "Steam Deck").
//
// A watcher thread ticks every 100 ms (resume_gap.h). Two ticks a second or more apart on
// CLOCK_MONOTONIC mean no thread of the game ran in between: the process was stopped (SIGSTOP,
// a debugger, something that pauses the game before the machine sleeps). On Linux,
// CLOCK_BOOTTIME moving ahead of CLOCK_MONOTONIC means the machine slept. Either is logged as
// FORZA_RESUME (a stop also shows as a FORZA_STALL "no frame" line). After a sleep Forza Live
// says HELLO at once instead of within 5 s (the server forgets a player it has not heard from
// for 30 s); after a stop its own thread notices the gap and does (net.cpp).
//
// The game's timing needs nothing: a system suspend stops the clock it runs on
// (CLOCK_MONOTONIC_RAW), so it sees no gap; after a stop it takes a frame over 4 s as one 60 Hz
// frame, the physics step takes at most 0.2 s of a longer frame, and the race clock moved the
// same 2.1 s across a 3 s stop as across a 10 s one, at 30 and at 60 fps.

#include "resume.h"
#include "resume_gap.h"
#include "../kernel/live.h"

#include <rex/logging.h>

#include <chrono>
#include <ctime>

namespace forza::resume {

namespace {

double Running() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Seconds the machine has slept since boot (Linux), else 0.
double Asleep() {
#ifdef __linux__
  timespec boot{}, mono{};
  if (clock_gettime(CLOCK_BOOTTIME, &boot) != 0 || clock_gettime(CLOCK_MONOTONIC, &mono) != 0) {
    return 0;
  }
  return double(boot.tv_sec - mono.tv_sec) + double(boot.tv_nsec - mono.tv_nsec) * 1e-9;
#else
  return 0;
#endif
}

void Watch(const std::atomic<bool>& stop) {
  GapDetector detector;
  detector.Tick(Running(), Asleep());
  while (!stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const Gap gap = detector.Tick(Running(), Asleep());
    if (gap.stopped > 0) {
      REXLOG_WARN("FORZA_RESUME: the game did not run for {:.1f} s (stopped)", gap.stopped);
    }
    if (gap.suspended > 0) {
      REXLOG_WARN("FORZA_RESUME: the machine slept for {:.0f} s", gap.suspended);
    }
    if (gap.suspended > 0) forza::LiveResumed(gap.suspended);
  }
}

}  // namespace

void Watcher::Start() {
  if (worker_.joinable()) return;
  stop_.store(false, std::memory_order_relaxed);
  worker_ = std::thread([this] { Watch(stop_); });
}

void Watcher::Stop() {
  if (!worker_.joinable()) return;
  stop_.store(true, std::memory_order_relaxed);
  worker_.join();
}

}  // namespace forza::resume
