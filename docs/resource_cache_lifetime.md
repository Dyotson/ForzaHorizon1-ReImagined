# TU4 cache eviction and incoming references (#182)

Issue #182 faults reading guest address `0x8` with LR `82D2EC34` and a
nonnull object in r3/r31 whose vtable has become zero. That return address is
the first pointer copy in `sub_82D2EB78`: cache slot -> resource+0x40.
The copy helper `sub_82471820` loads a pointer before calling its virtual
retain function at vtable+8. The cache owns the reference that must keep that
pointer alive until the reader retains it.

Lookups hold the cache's reader lock at +112. The cache's sole-owner eviction,
`sub_82D25DD8`, checks the object's reference count at +32, clears its slot and
releases it without acquiring the corresponding writer lock. It is called by
the global trimming loop while that loop owns the eviction-list critical
section. This permits a reader to load the sole-owned object immediately
before trimming destroys it, then retain through freed memory. A controlled
replay of the actual generated TU4 copy/replacement instructions reproduces
the reported zero-vtable read of `0x8` and LR `82D2EC34`. That demonstrates a
compatible lifetime race, not the unrecorded history of the reporter's object.

The TU4 eviction wrapper tries the existing writer-owner critical section at
cache+120, then claims cache+112 only if the reader counter is zero. A busy
cache returns zero bytes reclaimed, so the existing trimming loop can continue
to another candidate. The original eviction's eligibility, cleanup, virtual
Release and byte-count result remain intact after successful acquisition.
The original guest writer unlock restores the reader count, wakes readers
that arrived during eviction, and releases the writer critical section.

The acquisition must not wait: lookup can hold the cache reader lock and then
request the eviction-list lock, while trimming holds the list lock. Blocking
for readers would reverse this order and deadlock. Failed attempts also leave
an existing writer or reader count unchanged. The exclusive owner lock is
still needed to exclude another writer in the gap before it marks the counter.

This is separate from #177, which fixes competing load/unload takes of the
destination resource+0x40. This change protects the incoming cache reference
against eviction; it is not a global smart-pointer replacement or a null-vtable
skip. Other publication/lifetime bugs remain possible. It applies only to the
verified TU4 executable; the disc build has no new override.

Set `FORZA_RESOURCE_LIFETIME_TRACE=1` for up to 20 diagnostics showing guarded
evictions or busy-cache deferrals. No logging is added to the normal hot path.

## Validation

The SDK-independent regression demonstrates the old stale-retain sequence,
checks the reader-pinned case, busy readers/writers, readers arriving during
eviction, and 10,000 concurrent read/retain vs sole-owner-eviction rounds.
It runs in Linux release CI and can be run locally with:

```sh
g++ -std=c++20 -O2 -pthread forza-recomp/tests/resource_cache_lock_test.cpp -o /tmp/resource-cache-test
/tmp/resource-cache-test
```

Reporter-specific online free-roam reproduction and before/after validation
remain necessary before claiming #182 resolved.
