// forza - telling that the game was stopped or the machine slept, from a watcher's ticks
// (src/platform/resume.cpp, #140). No runtime in here: forza-recomp/tests/resume_gap_test.cpp.

#pragma once

namespace forza::resume {

struct Gap {
  double stopped = 0;    // seconds no thread of the game ran while the machine was awake
  double suspended = 0;  // seconds the machine slept
};

// The watcher ticks about every 100 ms. `running` is a clock that runs on while the process
// is stopped but not while the machine sleeps (CLOCK_MONOTONIC), `asleep` the time the machine
// has slept since boot (CLOCK_BOOTTIME - CLOCK_MONOTONIC), both in seconds. Two ticks
// `min_seconds` or more apart on `running` mean the process was stopped (SIGSTOP, a debugger,
// a suspend that pauses the game first) or starved; `asleep` growing that much, a system
// suspend. Under `min_seconds` is neither (a busy machine).
class GapDetector {
 public:
  explicit GapDetector(double min_seconds = 1.0) : min_(min_seconds) {}

  Gap Tick(double running, double asleep) {
    Gap gap;
    if (started_) {
      if (running - running_ >= min_) gap.stopped = running - running_;
      if (asleep - asleep_ >= min_) gap.suspended = asleep - asleep_;
    }
    started_ = true;
    running_ = running;
    asleep_ = asleep;
    return gap;
  }

 private:
  const double min_;
  bool started_ = false;
  double running_ = 0, asleep_ = 0;
};

}  // namespace forza::resume
