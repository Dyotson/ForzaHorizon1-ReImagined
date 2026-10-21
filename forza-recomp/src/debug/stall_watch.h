// forza - log long frames and where the main loop and render threads were during them
// (src/debug/stall_watch.cpp, #187). Always on.

#pragma once

#include <atomic>
#include <thread>

namespace forza::stall {

// Owned by the app so the worker cannot outlive the runtime it samples.
// Start/Stop are called on the app's lifecycle thread. Destruction also joins.
class Watchdog {
 public:
  ~Watchdog() { Stop(); }
  void Start();
  void Stop();

 private:
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

// The render thread handed a frame to VdSwap (src/debug/frame_dump.cpp).
void OnGuestSwap();

// The main loop took its frame time (src/hooks/frame_time.cpp).
void OnMainFrame();

}  // namespace forza::stall
