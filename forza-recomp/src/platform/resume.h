// forza - a game that was stopped or whose machine slept (src/platform/resume.cpp, #140): it
// says so in the log, and Forza Live signs in again at once.

#pragma once

#include <atomic>
#include <thread>

namespace forza::resume {

// Owned by the app, started and stopped with the stall watchdog.
class Watcher {
 public:
  ~Watcher() { Stop(); }
  void Start();
  void Stop();

 private:
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

}  // namespace forza::resume
