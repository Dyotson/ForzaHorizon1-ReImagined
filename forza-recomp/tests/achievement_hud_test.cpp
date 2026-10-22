// Standalone C++17: visible time, queue order and concurrent notification delivery.
#include "../src/platform/achievement_toast.h"
#include <cassert>
#include <thread>
#include <vector>

int main() {
  using forza::achievement_hud::Queue;
  Queue q;
  assert(!q.Read(0));
  q.Push(1, "Welcome to Horizon", 10);
  q.Push(1, "Duplicate notification", 10);
  q.Push(2, "Second unlock", 20);
  // No native HUD yet: ten minutes of loading must not expire queued unlocks.
  assert(q.Read(600000)->remaining == 4500);
  assert(q.Read(600000)->name == "Welcome to Horizon");
  const auto first = q.Read(600000)->revision;
  q.Resume(600000);
  q.Resume(601000);  // repeated frames do not restart the display duration
  assert(q.Read(601500)->remaining == 3000);
  q.Pause(601500);   // ordinary notification/challenge/unbind
  q.Pause(700000);
  assert(q.Read(900000)->remaining == 3000);
  q.Resume(900000);
  assert(q.Read(902999)->id == 1);
  assert(!q.Read(903000));  // HIDE before showing the next item
  assert(q.Read(903001)->id == 2);
  assert(q.Read(903001)->gamerscore == 20);
  assert(q.Read(903001)->revision != first);
  assert(q.Read(903001)->remaining == 4500);
  q.Resume(903001);
  assert(!q.Read(907501));
  assert(!q.Read(907502));  // duplicate ID 1 was not queued twice
  q.Push(1, "Explicit replay", 10);  // a completed notification may be requested again
  assert(q.Read(907503)->id == 1);
  q.Clear();
  assert(!q.Read(907504));
  q.Resume(907504);  // empty resume does not consume time for the next event
  q.Push(3, "New session", 0);
  assert(q.Read(1000000)->remaining == 4500);
  q.Clear();

  // Concurrent callbacks cannot lose distinct unlocks or enqueue duplicates.
  std::vector<std::thread> writers;
  for (uint32_t thread = 0; thread < 4; ++thread)
    writers.emplace_back([&] { for (uint32_t id = 1; id <= 100; ++id) q.Push(id, "Unlock", id); });
  for (auto& writer : writers) writer.join();
  bool seen[101]{};
  for (int count = 0; count < 100; ++count) {
    const auto notice = *q.Read(count * 5000);
    assert(notice.id >= 1 && notice.id <= 100 && !seen[notice.id]);
    seen[notice.id] = true;
    q.Resume(count * 5000);
    assert(!q.Read(count * 5000 + 4500));
  }
  assert(!q.Read(1000000));
}
