// Exercise the production worker against a blocked guest stack read, without a game/SDK.
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "../src/debug/stall_watch.cpp"
#include <cassert>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

namespace {
std::mutex gate_mutex;
std::condition_variable gate;
bool entered = false, released = false;
int reads = 0;
}

namespace forza {
std::string DescribeGuestThread(rex::system::XThread&) {
  std::unique_lock lock(gate_mutex);
  ++reads;
  entered = true;
  gate.notify_all();
  gate.wait(lock, [] { return released; });
  return "guest stack";
}
}

int main() {
  using namespace std::chrono_literals;
  using namespace forza::stall;
  for (bool destroy : {false, true}) {
    {
      std::lock_guard lock(gate_mutex);
      entered = released = false;
      reads = 0;
    }
    // Put the real watchdog past its boot grace, with a long frame to sample.
    g_first_swap = Now() - kGrace - kStall;
    g_last_swap = Now() - kStall;
    g_main_thread = 1;
    g_render_thread = 2;
    auto watchdog = std::make_unique<Watchdog>();
    watchdog->Stop();  // Shutdown can happen before the module starts.
    watchdog->Start();
    watchdog->Start();  // Repeated startup must not launch another worker.
    {
      std::unique_lock lock(gate_mutex);
      assert(gate.wait_for(lock, 5s, [] { return entered; }));
    }
    std::promise<void> stopping;
    auto started = stopping.get_future();
    auto stopped = std::async(std::launch::async, [&] {
      stopping.set_value();
      if (destroy) watchdog.reset();
      else watchdog->Stop();
    });
    started.wait();
    // Neither explicit Stop nor app-member destruction may return while a sample
    // still holds guest memory. This fails for the former detached worker.
    assert(stopped.wait_for(100ms) == std::future_status::timeout);
    {
      std::lock_guard lock(gate_mutex);
      released = true;
    }
    gate.notify_all();
    assert(stopped.wait_for(5s) == std::future_status::ready);
    stopped.get();
    if (watchdog) watchdog->Stop();  // Both shutdown callbacks may run.
    int completed_reads;
    {
      std::lock_guard lock(gate_mutex);
      completed_reads = reads;
      assert(completed_reads == 2);  // One worker, main and render only.
    }
    // A new stalled frame must not be sampled after shutdown returns.
    g_last_swap = Now() - kStall;
    std::this_thread::sleep_for(150ms);
    {
      std::lock_guard lock(gate_mutex);
      assert(reads == completed_reads);
    }
  }
}
