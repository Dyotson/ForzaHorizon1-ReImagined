# TU4 cache eviction and incoming references (#182)

Issue #182 faults reading guest address `0x8` with LR `82D2EC34` and a
nonnull object in r3/r31 whose vtable has become zero. That return address is
the first pointer copy in `sub_82D2EB78`: cache slot -> resource+0x40.
The copy helper `sub_82471820` loads a pointer before calling its virtual
retain function at vtable+8. The cache owns the reference that must keep that
pointer alive until the reader retains it.

Lookups hold a cache reader lock. Sole-owner eviction (`sub_82D25DD8`)
checks the object's reference count at +32, clears its slot and releases it
without excluding that pointer load/retain. A controlled replay of actual
TU4 instructions, with modeled reference-count callbacks, reproduces the
reported zero-vtable read of `0x8` and LR `82D2EC34`. This demonstrates a
compatible lifetime race, not the unrecorded history of the reporter's object.

## Short pin gate

The verified TU4 cache-copy call sites use a host mutex to protect only the
source pointer load and original virtual AddRef. Once retained, the reference
owns its lifetime. Publication atomically exchanges the destination pointer,
then calls the outgoing object's original virtual Release outside the gate.
This preserves null/self-assignment ownership and cooperates with existing
atomic load/unload takes. Virtual calls use an aligned guest stack frame and
a private register context; the helper still returns its destination pointer.
Unrelated callers of the generic copy helper use its original implementation.

Eviction tries the same mutex. An active pin returns zero bytes reclaimed;
otherwise the original eviction runs under the gate, preserving its original
eligibility, cleanup, virtual Release and reclaimed-byte result. The original
sole-owner check skips objects retained by a reader.

The trim caller owns the global eviction-list lock, so eviction must never
wait for a pin that might need that list. Publication and outgoing Release
are deliberately outside the pin gate because they can reenter allocation.
The cache objects' verified virtual AddRef only increments their reference.

The gate must not depend on the entire guest cache reader count reaching
zero: allocation can request trimming while its own caller holds that reader
lock. Doing so can prevent all reclamation and leave allocation retrying even
though drawing continues. The short gate permits reclamation in that state;
validation checks reclaimed bytes and streaming progress, not only FPS.

This complements #177's competing destination load/unload takes by protecting
the incoming cache reference and its destination handoff. It is not a global
smart-pointer replacement or a null-vtable skip. Other publication/lifetime
bugs remain possible. Overrides apply only to the verified TU4 executable;
the disc build has no new override.

Set `FORZA_RESOURCE_LIFETIME_TRACE=1` for up to 20 diagnostics per outcome:
guarded evictions and active-pin deferrals. Independent limits keep startup
deferrals from hiding guarded evictions. Logging is disabled by default.

## Validation

The SDK-independent regression covers the old stale-retain schedule, a pin
paused between load and retain, reclamation while an outer reader exists,
self/null assignment, and outgoing Release reentering trimming. It also races
one reader against two eviction writers for 10,000 rounds and destination
publication against an atomic unload for 10,000 rounds.

It runs in Linux release CI and can be run locally with:

```sh
g++ -std=c++20 -O2 -pthread forza-recomp/tests/resource_cache_lock_test.cpp -o /tmp/resource-cache-test
/tmp/resource-cache-test
```

The PR description records the tested commit, game runtime provenance and
completed runtime checks. CI does not compile or run the player-owned game.
Reporter-specific online free-roam reproduction and before/after validation
remain necessary before claiming #182 resolved.
