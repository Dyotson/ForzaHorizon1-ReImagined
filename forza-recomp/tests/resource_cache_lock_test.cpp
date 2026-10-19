#include "../src/hooks/resource_cache_lock.h"
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "Failed line %d: %s\n", __LINE__, #c); std::abort(); } } while (false)
using forza::resource::GuestWord;
using forza::resource::TryClaimIdleCache;

static uint32_t Add(uint32_t& word, uint32_t increment) {
  std::atomic_ref<uint32_t> counter(word);
  auto before = counter.load();
  for (;;) {
    const auto after = GuestWord(GuestWord(before) + increment);
    if (counter.compare_exchange_weak(before, after)) return GuestWord(after);
  }
}

int main() {
  // Previously a reader could load A, eviction saw its sole cache reference and
  // destroyed it, then the reader called AddRef through A's zero/freed vtable.
  int references = 1;
  bool vtable_live = true;
  const bool reader_loaded_pointer = true;
  if (--references == 0) vtable_live = false;
  CHECK(reader_loaded_pointer && !vtable_live);

  // The cache's existing reader acquisition now prevents that eviction. Once
  // pinned, the caller keeps A alive even after eviction drops the cache owner.
  uint32_t counter = GuestWord(1);
  references = 1; vtable_live = true;
  CHECK(!TryClaimIdleCache(counter));
  CHECK(counter == GuestWord(1));
  ++references;
  CHECK(Add(counter, uint32_t(-1)) == 0);
  CHECK(TryClaimIdleCache(counter));
  CHECK(--references == 1 && vtable_live);
  CHECK(Add(counter, 0x80000000u) == 0);

  // Never overwrite an existing reader, writer or writer-with-waiting-readers.
  for (auto value : {1u, 2u, 0x7fffffffu, 0x80000000u, 0x80000001u}) {
    counter = GuestWord(value);
    CHECK(!TryClaimIdleCache(counter));
    CHECK(counter == GuestWord(value));
  }

  // New readers can arrive after a writer claimed an idle cache. Their negative
  // count makes them wait; the original writer release restores the count and
  // wakes exactly these readers rather than letting them inspect retired data.
  counter = 0;
  CHECK(TryClaimIdleCache(counter));
  CHECK(Add(counter, 1) == 0x80000001u);
  CHECK(Add(counter, 0x80000000u) == 1);
  CHECK(Add(counter, uint32_t(-1)) == 0);

  // Race real read/pin/release and sole-owner eviction for distinct publications.
  // The owner critical section models the guest writer mutex; eviction never
  // waits for readers while holding the outer eviction-list lock.
  constexpr int rounds = 10000;
  std::barrier phase(4);
  std::mutex writer_owner;
  std::atomic<int> refs{0};
  std::atomic<bool> live{false}, cached{false};
  counter = 0;
  auto release = [&] { if (refs.fetch_sub(1) == 1) live.store(false); };
  std::thread reader([&] {
    for (int i = 0; i < rounds; ++i) {
      phase.arrive_and_wait();
      const auto value = Add(counter, 1);
      if (int32_t(value) < 0)
        while (int32_t(GuestWord(std::atomic_ref<uint32_t>(counter).load())) < 0) std::this_thread::yield();
      bool pinned = cached.load();
      if (pinned) {
        CHECK(live.load());
        CHECK(refs.fetch_add(1) > 0);
      }
      Add(counter, uint32_t(-1));
      if (pinned) release();
      phase.arrive_and_wait();
    }
  });
  auto eviction = [&] {
    for (int i = 0; i < rounds; ++i) {
      phase.arrive_and_wait();
      if (writer_owner.try_lock()) {
        if (TryClaimIdleCache(counter)) {
          if (refs.load() == 1 && cached.exchange(false)) release();
          Add(counter, 0x80000000u);
        }
        writer_owner.unlock();
      }
      phase.arrive_and_wait();
    }
  };
  std::thread evict(eviction), competing_writer(eviction);
  for (int i = 0; i < rounds; ++i) {
    refs.store(1); live.store(true); cached.store(true);
    phase.arrive_and_wait();phase.arrive_and_wait();
    CHECK(counter == 0);
    CHECK(refs.load() == (cached.load() ? 1 : 0));
    CHECK(live.load() == cached.load());
  }
  reader.join();evict.join();competing_writer.join();
  std::puts("cache eviction: stale-retain schedule, busy states, waiting readers and 10000 reader/two-writer races passed");
}
