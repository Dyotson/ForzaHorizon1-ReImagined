# SDK guest write regression

This standalone test uses a real SDK memory instance and its invalidation callbacks.
It needs neither game data nor a GPU. Keep the SDK's `bin` directory on the runtime
library search path, including its DLL dependencies on Windows.

```sh
cmake -S forza-recomp/tests/guest-write -B build/guest-write -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build build/guest-write --parallel 1
ctest --test-dir build/guest-write --output-on-failure
```

The test covers host read-only pages that remain writable to the guest, real guest
read-only protection, reserve-only memory, unowned addresses, mixed page protection,
cross-page watches, and a callback that changes guest permission before the write.
The helper calls the SDK's physical-memory invalidation API, then checks current
guest permissions and every touched host page again. It never changes host
protection directly or writes through another alias.

Callers must enter without an existing SDK global-region lock. A successful
transaction retains that lock until `End()` or destruction. The caller must
revalidate its native owner, UID, revision, cursor, flags and capacity after
`Begin()`, serialize lifecycle cancellation, write the complete payload, and
commit the cursor last. Guest page permission alone does not prove native ownership.

These headers and the regression were exercised against the private SDK candidate
`0.10.0.166-dev.gbd9c98c`. The public camera does not use this helper yet. The tested
private writer still rejects ring-generation changes and reservations requiring
unproved native slow allocation. Persistent scenery removal, a complete pan, and
world-before-UI fade ordering remain unqualified. This commit does not enable them.
