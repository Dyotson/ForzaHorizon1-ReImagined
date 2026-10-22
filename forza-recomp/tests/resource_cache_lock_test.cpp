#include "../src/hooks/resource_cache_lock.h"
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #c); std::abort(); } } while (false)
using forza::resource::CachePinGate;

struct Object { std::atomic<int> refs{1}; std::atomic<bool> live{true}; };
static void Retain(Object* p) { CHECK(p->live.load()); CHECK(p->refs.fetch_add(1) > 0); }
static void Release(Object* p) { const auto before = p->refs.fetch_sub(1); CHECK(before > 0); if (before == 1) p->live.store(false); }

int main() {
  CHECK(forza::resource::IsCacheCopyReturn(0x82D2EC34));
  CHECK(!forza::resource::IsCacheCopyReturn(0x82753790));
  // The old schedule destroys the sole cache reference before the loaded
  // pointer can be retained, reproducing a stale/zero vtable.
  Object old;
  auto* loaded = &old;
  Release(&old);
  CHECK(!loaded->live.load());

  CachePinGate gate;
  Object pinned;
  Object* destination = nullptr;
  std::latch pointer_loaded(1), continue_pin(1);
  std::thread pin([&] {
    gate.Copy([&] { pointer_loaded.count_down(); continue_pin.wait(); return &pinned; },
              Retain, [&](Object* p) { auto* before = destination; destination = p; return before; }, Release);
  });
  pointer_loaded.wait();
  CHECK(!gate.TryEvict());
  continue_pin.count_down();pin.join();
  CHECK(destination == &pinned && pinned.refs == 2 && pinned.live);
  Release(destination);

  // Allocation may trim while its caller still owns a guest cache reader.
  // It must reclaim sole-owned entries when no load/retain is in flight.
  const int outer_guest_readers = 1;
  {
    auto eviction = gate.TryEvict();
    CHECK(eviction && outer_guest_readers == 1);
    Release(&pinned);
  }
  CHECK(!pinned.live);

  // Null and self assignment preserve ownership. The outgoing Release is
  // outside the gate, so reentrant allocation/eviction can make progress.
  Object same;
  destination = &same;
  auto publish = [&](Object* p) { auto* before = destination; destination = p; return before; };
  auto outgoing = [&](Object* p) { auto eviction = gate.TryEvict(); CHECK(eviction); Release(p); };
  gate.Copy([&] { return destination; }, Retain, publish, outgoing);
  CHECK(destination == &same && same.refs == 1);
  gate.Copy([]() -> Object* { return nullptr; }, Retain, publish, outgoing);
  CHECK(!destination && !same.live);

  constexpr int rounds = 10000;
  std::barrier phase(4);
  Object object;
  Object* cached = nullptr;
  std::thread reader([&] {
    for (int i = 0; i < rounds; ++i) {
      phase.arrive_and_wait();
      Object* local = nullptr;
      gate.Copy([&] { return cached; }, Retain,
                [&](Object* p) { local = p; return static_cast<Object*>(nullptr); }, Release);
      if (local) Release(local);
      phase.arrive_and_wait();
    }
  });
  auto trim = [&] {
    for (int i = 0; i < rounds; ++i) {
      phase.arrive_and_wait();
      {
        auto eviction = gate.TryEvict();
        if (eviction && cached && object.refs.load() == 1) {
          cached = nullptr;Release(&object);
        }
      }
      phase.arrive_and_wait();
    }
  };
  std::thread writer(trim), other_writer(trim);
  for (int i = 0; i < rounds; ++i) {
    object.refs = 1;object.live = true;cached = &object;
    phase.arrive_and_wait();phase.arrive_and_wait();
    CHECK(object.refs.load() == (cached ? 1 : 0));
    CHECK(object.live.load() == bool(cached));
  }
  reader.join();writer.join();other_writer.join();

  // Publication races the resource's existing atomic unload take. Each old
  // owner must be released once, and an unloaded new owner must not be lost.
  std::barrier publication_phase(3);
  Object incoming, previous;
  std::atomic<Object*> field{nullptr};
  std::thread publisher([&] {
    for (int i = 0; i < rounds; ++i) {
      publication_phase.arrive_and_wait();
      gate.Copy([&] { return &incoming; }, Retain,
                [&](Object* p) { return field.exchange(p); }, Release);
      publication_phase.arrive_and_wait();
    }
  });
  std::thread unloader([&] {
    for (int i = 0; i < rounds; ++i) {
      publication_phase.arrive_and_wait();
      if (auto* p = field.exchange(nullptr)) Release(p);
      publication_phase.arrive_and_wait();
    }
  });
  for (int i = 0; i < rounds; ++i) {
    incoming.refs = 1;incoming.live = true;
    previous.refs = 1;previous.live = true;field = &previous;
    publication_phase.arrive_and_wait();publication_phase.arrive_and_wait();
    CHECK(previous.refs == 0 && !previous.live);
    CHECK(incoming.refs.load() == (field.load() ? 2 : 1));
    if (auto* p = field.exchange(nullptr)) Release(p);
    Release(&incoming);
    CHECK(!incoming.live);
  }
  publisher.join();unloader.join();
  std::puts("cache pins: stale retain, active pin exclusion, allocation progress, self/null assignment, 10000 reader/two-writer races and 10000 publication/unload races passed");
}
