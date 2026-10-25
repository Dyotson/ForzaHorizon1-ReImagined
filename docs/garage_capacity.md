# Garage capacity investigation

Status: source audit in progress, 2026-10-25. No capacity change is enabled.
The audit started at main `0e1717ba898bc4fc086b2cb51dcd7721a9445e8a`.
The existing native pause-menu work is separate.

## Confirmed stock behavior

The stock garage limit is **550**, including additions waiting in the native
queue. TU4's full predicate is `8259DC10`; the matching disc/TU0 predicate is
`8252FCD0`. Both read the queue's begin/end pointers at object offsets 68/72,
divide the byte difference by eight, add the database row count, and report full
at 550 or more. The TU4 count helper `8259DB60` executes a count query against
the garage object's table name. A higher limit must retain pending additions in
the calculation: checking only rows already committed permits overbooking.

All nine direct TU4 predicate call sites have been enumerated:

| Caller | Call site | Observed use |
| --- | --- | --- |
| `825C8478` | `825C84B4` | Central native car-add routine; rejects a full garage before allocating a car and ID. |
| `825D1918` | `825D193C` | Full-garage replacement path: selects an existing car, removes it, then adds car 309. Its higher-level trigger still needs classification. |
| `82682E30` | `82682F48` | CarFlow screen with the localized buy-car/full-garage message. |
| `8273D808` | `8273D878` | Barn-find/workshop flow with a full-garage message. |
| `827A20A8` | `827A28A4` | Message Center/gift preparation flow with a full-garage message. |
| `8282F3E0` | `8282F468` | Tombola flow: stores full status and controls notifications. |
| `82852790` | `828527C0` | Car transition that selects a cash-prize/full response; exact screen identity remains open. |
| `828E4710` | `828E4750` | Active-profile wrapper returning the full predicate. |
| `82918B68` | `82918BDC` | Acquisition eligibility gate that also checks storage-device free space. |

The last caller's separate 2 MiB device-free-space requirement is not a garage
row limit. Its chain reaches `XamContentGetDeviceData`, via volume method
`82D208B0` and import thunk `82B2AFF8` / `832B863C`. Preserve this check.

A second literal 550 occurs in `82E2C780`, which calls `82E3E4E8` to allocate
550 elements of 240 bytes and 100 elements of 60 bytes. RTTI identifies the
owner as `CTrackPresentation`, with Default, Null and Unified presentation
subclasses. This is a track presentation pool, not evidence of garage backing
storage, and must not be changed as part of a garage limit patch.

## Storage, IDs and thumbnails

The garage object constructor `825B4970` allocates no fixed 550-car array. It
initializes a dynamic pending vector at offsets 68/72/76. Its destructor
`825B49E8` destroys that vector. The drain routine `825C9318` pops an eight-byte
entry before forwarding it through the native add path, so a drained entry is
not counted twice by the full predicate. Queue insertion/growth still needs a
complete audit.

Native IDs are database-backed. `825B18F8` considers both `LastUsedGarageId` and
the maximum saved garage ID, advances the counter, and updates the database.
`825BFE60` constructs the native row and inserts it. This does not establish
safe behavior at integer exhaustion; the launcher already checks its supported
signed ID range before adding cars.

The audited car-list loader `8253D7B8` sizes an 80-byte-per-row vector from the
query's row count, then iterates results. Its resize/growth helpers
`8253CAC8` / `825372C8` use dynamic allocation with overflow checks. This path
has no observed 550-element allocation; other list/sort/filter paths remain to
be checked and measured.

Thumbnail names are constructed through `825B60C0`. Cleanup `825C1630` queries
saved IDs in order, builds a dynamic set, enumerates thumbnail files, and removes
unreferenced files. Native removal `825B1290` deletes the car row and its
purchased-parts rows and handles its thumbnail. Rendering, cache pressure and
delete/re-add behavior above 550 still require runtime verification.

The profile-volume constructor `825EFD28` consumes filename/size pairs from
`83305074`. Initializer `83221270` assigns `PlayerDatabase` to the second pair,
whose size is `0x600000` (6 MiB). Profile loading `82583178` and volume creation
`82565888` use this configuration. This agrees with the copied baseline's
6,291,464-byte outer save file (6 MiB plus its eight-byte header).

The authenticated baseline contains three garage rows in a 29,696-byte SQLite
database. This is not a worst-case headroom measurement. Purchased upgrades,
career progress, strings, and compression affect the eventual save size.
The existing launcher encoder preserves the allocated container size and
refuses an edit that will not fit. Its 32 MiB decode safety ceiling is not the
native save allocation and must not be used to justify a larger garage.

The launcher's existing authenticated unicorn editor currently has no logical
550-row check. Any supported limit policy must also cover that editor. Preserve
its game-data-derived stock builder, authenticated decode/encode, ID guard,
full backup, concurrency checks, and atomic replacement. Do not manufacture
boundary saves by changing arbitrary garage fields.

## Proposed implementation and evidence gates

First candidate: an optional **1,000**-car limit, retaining **550** as the default
until testing establishes compatibility. Only validated choices should be
accepted; no unbounded setting. This is a proposal, not a supported limit yet.

Use one policy for the central native full predicate and launcher additions,
with TU0/TU4 address mapping. Keep the original full behavior at the default,
all storage-space checks, native add/remove and save functions, and fail-closed
handling for invalid counts or queue pointers. Changing the full predicate
would reach all nine callers; patching individual acquisition screens would
leave inconsistent behavior. Do not alter the save format or enlarge its
container as an incidental part of this change.

Before implementation, finish queue insertion, import/gift quotas, list variants,
and serialization failure propagation. Establish full-save headroom at the
candidate size, including substantial purchased-parts and career data. If a
valid populated profile cannot fit, lower or withdraw the candidate.

Disposable copied profiles must then cover 549/550/551 and 999/1000/1001,
including pending additions. Exercise actual native add/purchase/reward paths,
save and normal exit, restart, car selection, thumbnails, sorting/filtering,
remove and re-add. Compare every saved ID/model and the ID counter across
restart; rejection must preserve credits, cars and existing files. A populated
profile loaded with the default limit must retain every existing car while
rejecting further additions. Check TU0 as well as TU4 and inspect timing/memory
changes. Source review and unit checks cannot replace these native boundaries.

Private decoded binaries, generated function extracts, profile copies, hashes
and audit metadata stay outside Git under `build/garage-capacity-20261025`.
No build or game launch has been performed for this investigation.
