// Checks SDK patch 0114 (rex::thread waits on Linux): a thread blocked on several handles, or in
// an alertable wait, wakes for its own handles and for user callbacks queued to it, not for every
// signal in the process, and alertable waits no longer poll every millisecond; a thread that
// exits inside such a wait does not hang a WaitAll that joins it.
// Build (CI, Linux SDK job): clang++ -std=c++23 -pthread -I<overlay>/include
//   tools/test_wait_wakeups.cpp -L<overlay>/lib -lrexruntime -Wl,-rpath,<overlay>/lib
#include <rex/thread.h>

#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Semaphore;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;
using Clock = std::chrono::steady_clock;

namespace {

int failures = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
      failures++;                                             \
    }                                                         \
  } while (0)

long VoluntarySwitches() {  // of the calling thread
  rusage usage{};
  getrusage(RUSAGE_THREAD, &usage);
  return usage.ru_nvcsw;
}

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

}  // namespace

int main() {
  // A WaitAny returns the handle that was signalled.
  {
    auto a = Event::CreateAutoResetEvent(false);
    auto b = Event::CreateAutoResetEvent(false);
    std::vector<WaitHandle*> hs{a.get(), b.get()};
    std::thread setter([&] {
      std::this_thread::sleep_for(5ms);
      b->Set();
    });
    auto r = rex::thread::WaitAny(hs, false, 5000ms);
    setter.join();
    CHECK(r.first == WaitResult::kSuccess && r.second == 1);
    CHECK(rex::thread::WaitAny(hs, false, 0ms).first == WaitResult::kTimeout);  // auto reset
  }
  // WaitAll waits for both; a timeout is not cut short.
  {
    auto a = Event::CreateManualResetEvent(false);
    auto b = Event::CreateManualResetEvent(false);
    std::vector<WaitHandle*> hs{a.get(), b.get()};
    auto t0 = Clock::now();
    CHECK(rex::thread::WaitAny(hs, false, 30ms).first == WaitResult::kTimeout);
    CHECK(Clock::now() - t0 >= 30ms);
    std::thread setter([&] {
      std::this_thread::sleep_for(3ms);
      a->Set();
      std::this_thread::sleep_for(3ms);
      b->Set();
    });
    CHECK(rex::thread::WaitAll(hs, false, 5000ms) == WaitResult::kSuccess);
    setter.join();
  }
  // Signals of other handles do not wake a multi-handle wait (patch 0033 woke every such wait
  // on every signal in the process).
  {
    auto a = Event::CreateAutoResetEvent(false);
    auto b = Event::CreateAutoResetEvent(false);
    auto other = Event::CreateAutoResetEvent(false);
    std::vector<WaitHandle*> hs{a.get(), b.get()};
    std::atomic<bool> stop{false};
    std::thread noise([&] {
      while (!stop) {
        other->Set();
        std::this_thread::sleep_for(50us);
      }
    });
    const long before = VoluntarySwitches();
    auto r = rex::thread::WaitAny(hs, false, 300ms);
    const long switches = VoluntarySwitches() - before;
    stop = true;
    noise.join();
    std::printf("multi-handle wait during 300 ms of unrelated signals: %ld switches\n", switches);
    CHECK(r.first == WaitResult::kTimeout);
    CHECK(switches < 100);  // a re-check every 20 ms at most
  }
  // Every release is consumed once by many threads waiting on several semaphores.
  {
    constexpr int kSems = 4, kWaiters = 6, kPerProducer = 20000;
    std::vector<std::unique_ptr<Semaphore>> sems;
    for (int i = 0; i < kSems; i++) sems.push_back(Semaphore::Create(0, 1 << 20));
    std::vector<WaitHandle*> hs;
    for (auto& s : sems) hs.push_back(s.get());
    std::atomic<int> consumed{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> waiters;
    for (int w = 0; w < kWaiters; w++) {
      waiters.emplace_back([&] {
        while (true) {
          if (rex::thread::WaitAny(hs, false, 50ms).first == WaitResult::kSuccess) {
            consumed++;
          } else if (stop) {
            return;
          }
        }
      });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kSems; p++) {
      producers.emplace_back([&, p] {
        for (int i = 0; i < kPerProducer; i++) {
          sems[p]->Release(1, nullptr);
          if ((i & 255) == 0) std::this_thread::yield();
        }
      });
    }
    for (auto& t : producers) t.join();
    const auto deadline = Clock::now() + 20s;
    while (consumed < kSems * kPerProducer && Clock::now() < deadline) std::this_thread::sleep_for(1ms);
    stop = true;
    for (auto& t : waiters) t.join();
    std::printf("semaphores: %d of %d releases consumed\n", consumed.load(), kSems * kPerProducer);
    CHECK(consumed == kSems * kPerProducer);
  }
  // Alertable waits end when a user callback is queued, and do not poll meanwhile.
  {
    auto ev = Event::CreateManualResetEvent(false);
    auto ev2 = Event::CreateManualResetEvent(false);
    std::atomic<int> ran{0}, phase{0};
    WaitResult single = WaitResult::kFailed, multi = WaitResult::kFailed;
    auto slept = rex::thread::SleepResult::kSuccess;
    Clock::duration t_single{}, t_multi{}, t_sleep{};
    long idle_switches = -1;
    auto thread = rex::thread::Thread::Create({}, [&] {
      const long before = VoluntarySwitches();
      rex::thread::AlertableSleep(200ms);  // nothing queued: sleeps it out
      idle_switches = VoluntarySwitches() - before;
      phase = 1;
      auto t0 = Clock::now();
      single = rex::thread::Wait(ev.get(), true, 5000ms);
      t_single = Clock::now() - t0;
      phase = 2;
      std::vector<WaitHandle*> hs{ev.get(), ev2.get()};
      t0 = Clock::now();
      multi = rex::thread::WaitAny(hs, true, 5000ms).first;
      t_multi = Clock::now() - t0;
      phase = 3;
      t0 = Clock::now();
      slept = rex::thread::AlertableSleep(5000ms);
      t_sleep = Clock::now() - t0;
    });
    for (int p = 1; p <= 3; p++) {
      while (phase < p) std::this_thread::sleep_for(1ms);
      std::this_thread::sleep_for(20ms);
      thread->QueueUserCallback([&] { ran++; });
    }
    rex::thread::Wait(thread.get(), false, 20000ms);
    std::printf("alertable: 200 ms sleep %ld switches; ended by a callback after %.1f / %.1f / %.1f ms\n",
                idle_switches, Ms(t_single), Ms(t_multi), Ms(t_sleep));
    CHECK(idle_switches >= 0 && idle_switches < 30);  // was one a millisecond
    CHECK(single == WaitResult::kUserCallback);
    CHECK(multi == WaitResult::kUserCallback);
    CHECK(slept == rex::thread::SleepResult::kAlerted);
    CHECK(ran == 3);
    CHECK(t_single < 1000ms && t_multi < 1000ms && t_sleep < 1000ms);
  }
  // Alertable sleeps keep their length, below a millisecond too.
  {
    for (auto us : {300, 2500}) {
      auto t0 = Clock::now();
      auto r = rex::thread::AlertableSleep(std::chrono::microseconds(us));
      CHECK(r == rex::thread::SleepResult::kSuccess);
      CHECK(Clock::now() - t0 >= std::chrono::microseconds(us));
    }
  }
  // A zero-timeout alertable wait sees a signalled handle (it timed out before looking).
  {
    auto ev = Event::CreateManualResetEvent(true);
    CHECK(rex::thread::Wait(ev.get(), true, 0ms) == WaitResult::kSuccess);
  }
  // A thread that exits from a user callback queued to its alertable multi-handle wait on E
  // unwinds that wait (its registration with E) after its own handle is signalled. Meanwhile a
  // WaitAll on {the thread, E} holds both handles' locks and joins the thread. The unwinding
  // must not need E's lock: with the first 0114 it did, and both threads hung (#286 review).
  {
    static std::atomic<bool> unwinding{false}, release_unwind{false};
    struct ExitGate {  // keeps the exiting thread inside its unwind until the join has begun
      ~ExitGate() {
        unwinding = true;
        while (!release_unwind) std::this_thread::sleep_for(1ms);
      }
    };
    std::atomic<bool> done{false};
    WaitResult result = WaitResult::kFailed;
    std::thread scenario([&] {
      auto shared = Event::CreateManualResetEvent(false);
      auto other = Event::CreateManualResetEvent(false);
      std::atomic<bool> waiting{false};
      auto worker = rex::thread::Thread::Create({}, [&] {
        waiting = true;
        std::vector<WaitHandle*> hs{shared.get(), other.get()};
        rex::thread::WaitAny(hs, true, 5000ms);
      });
      while (!waiting) std::this_thread::sleep_for(1ms);
      std::this_thread::sleep_for(30ms);
      worker->QueueUserCallback([] {
        ExitGate gate;
        rex::thread::Thread::Exit(0);
      });
      while (!unwinding) std::this_thread::sleep_for(1ms);
      shared->Set();
      std::thread release([] {
        std::this_thread::sleep_for(50ms);
        release_unwind = true;
      });
      std::vector<WaitHandle*> both{worker.get(), shared.get()};
      result = rex::thread::WaitAll(both, false, 1000ms);
      release.join();
      done = true;
    });
    const auto deadline = Clock::now() + 5s;
    while (!done && Clock::now() < deadline) std::this_thread::sleep_for(5ms);
    if (!done) {
      std::printf("FAIL: WaitAll on an exiting thread and an event it waited on hung (deadlock)\n");
      std::fflush(stdout);
      std::_Exit(1);  // the hung threads cannot be joined
    }
    scenario.join();
    std::printf("thread exit during a WaitAll that joins it: WaitAll returned %d\n", int(result));
    CHECK(result == WaitResult::kSuccess);
  }
  std::printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
