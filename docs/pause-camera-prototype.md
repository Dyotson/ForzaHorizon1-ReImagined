# Pause camera experiment (TU4)

This is an incomplete, default-off **camera-only source prototype**, not a qualified
studio pause background. Enable with `--forza_pause_camera=true` in a TU4 build
with the native pause composition enabled and prepared. The option description
deliberately says camera only. No fresh in-game evidence exists yet.

## Original FH1 paths investigated

| Path | Evidence and ownership |
| --- | --- |
| Garage / Autoshow / paint car-target camera | `hooks/garage_camera.cpp`: `CUI4CarTargetCamera::Update` calls the transform at disc `82653584` / TU4 `826FD0D4`. Camera `+150` is state; only state 2 automatically orbits. State 1 moves to a preset, including the paint shop sides; its final state-0 frame shares that call. Angle `+80`, timer `+70`. This is a frontend owner, not the driving director. |
| Animated frontend shots | Same hook: `CUI4AnimatedCamera` caller disc `826499D4` supplies time to `CTSCCamera` / `CCamAnimPlayer`. Existing elapsed-time correction already avoids frame-rate-dependent shots. |
| Authored garage framing | The locally extracted original `media/UICarTargetCameraPresets.xml` names Autoshow, photo, upgrade and livery presets. These require their frontend camera/controller context. No original XML or scene bytes are added here. |
| Showroom rendering | Original `media/renderscenarios.zip`: `UI_Homespace.xml` selects tiling, UI car shaders and damage settings. `UI_Showroom.xml`, `UI_Car.xml`, `PhotoMode.xml` are separate original scenarios. Their existence does not establish that a car can be extracted from the live driving scene. Loading a homespace is a state/world transition, not a harmless pause-camera switch. |
| Driving camera | `docs/vr.md`, `hooks/vr_head_pose.cpp`: world update disc `823E3218` / TU4 `824131A8` -> split-view update `825AA1B0` / `826223B0` -> director update `82858288` / `82908710`. Director `+20` is active camera; `+240` is the completed world-to-camera matrix; `+304` is FOV. |
| Presentation and visibility | Matrix readers disc `825A9D20` / TU4 `82621F20` and disc `829A4860` / TU4 `82A58DA0`. The first queues the matrix to the existing deferred presentation camera and render-side view state; the second builds the game-side frustum. Existing VR scopes temporarily replace then restore this matrix. The prototype shares these scopes; it adds no renderer or duplicate guest hook. |
| Pause ownership | TU4 native pause bind `827DFB18`, tick `827DE0D0`, show `827DAB28`, teardown `827D6248`. Native composition lifecycle gates the experiment. World update hook `823E40D8` / TU4 `82414068` observes world pause byte `+0C`, started byte `+0D`, time scale `+28`. No world field is changed. |
| FOV compatibility | PR #437 moves FOV adjustment to accessor disc `8284EF90` / TU4 `828FF260`, covering seven calls. This experiment does not change FOV, projection, that accessor, or #437's source. Camera zoom is a dolly. True OpenXR cockpit mode and the test head pose both bypass the effect. |
| Native wheel camera | TU4 `CCamWheel` vtable `820861FC`, update `8290D098`, resolves the vehicle's dynamics reference through `8250AF98` (vehicle `+2D90`). It queries wheel radius at virtual `+228`, hub at virtual `+318`, then applies camera-target virtual `+20` to that car-space point. |
| Native wheel data | RTTI for `TRefCountedObjectThreadSafe<CCarDynamics>` has a secondary interface at object offset `AD0`, vtable `82003F0C`. Its `+228` function `82DDF248` reads radius at interface `+12A8 + wheel*410`; its `+318` function `82DDFFF8` copies hub vector at `+1350 + wheel*410`. Adapter requires that exact TU4 vtable, readable pages and finite plausible radii/hubs. No vtable dispatch or dynamics write is introduced. |
| Car-space transform | Existing `world_challenge.cpp` documents observed up at dynamics `-A50`, forward at `-A40`, world position at `-A20`. Original TU4 position getter `82DEA940` copies `-A20`; basis reader `82DE01C8` copies three rows starting at `-A60`; wheel transform/velocity function `82DDF2F0` multiplies hub coordinates by those basis rows. The adapter reconstructs right from validated orthonormal up/forward. Native tuning's `MatchesLivePressure` also distinguishes wheel indices 0/1 (front) from 2/3 (rear); geometry selection avoids depending on a left/right numeric convention. |

## Implemented source behavior

`pause_camera_transition.h` owns elapsed transition time and an immutable anchor:
one-second eased approach to an authored rear-quarter close-up, short hold,
1.6-second pullback, then a slow bounded pan. It emits a separate eased background
envelope for a future compositor. Long stalls freeze the transition instead of
skipping the close-up. The close-up targets the native rear hub selected from four
validated car-space wheel coordinates; the distance from that hub is authored.
The local-to-world mapping is supported by original accessors and existing
observations, but **has not yet been checked against an actual runtime image**.
Unknown layouts retain the original view rather than use a generic wheel offset.

`pause_camera.cpp` uses the existing local-car observation chain, bounded readable
page checks, and exterior chase-camera vtables only. It requires a fresh, visible,
validated native pause tick, offline Colorado free roam, a loaded paused world,
one local car, and one split view. Cockpit/hood/bumper/blended/photo/replay/menu
cameras, network play, tilted cars, changed car/director/anchor and invalid memory
fall back. TU0 and builds without native pause have no-op adapters.

The matrix is replaced only during each native reader and restored byte-for-byte
if the consumer has not written a newer matrix. No director camera pointer, FOV,
projection, car state, camera physics, profile, rewards or save data is changed.
Hide, child-screen entry and composition release cancel before native transitions.
Resume has no camera animation running over resumed simulation. A stale tick also
stops publication. New pause ownership starts from the new original driving view.

`forza_pause_camera_trace=true` enables bounded activation diagnostics while the
experiment is enabled. It logs each reason once per lifecycle/presentation/frustum
site, at most 64 records per process. Records include pause/world pulse ages,
the eligibility mask (enabled=1, free roam=2, offline=4), failed guard and observed
guest identities or flags. It adds no guest calls or writes and does not relax
the offline, VR, freshness, single-view or geometry guards. A camera-context
rejection still blocks that pause until its ownership is reset.

The latest production-host runtime attempt displayed exterior chase, native
pause and ordinary resume with both feature flags verified on, but never emitted
the camera-begin message. A subsequent 100.391-second diagnostic run identified
the first rejection: wheel 0's native getter value was `0x3DFAE148` (about 0.1225),
below the adapter's authored 0.15 minimum. Pause eligibility was 7, its world
observation was fresh and the presentation consumer ran during the pause.
The exact native vtable slot points directly at `82DDF248`, without a secondary
interface adjustment thunk. The shot does not use this value to position the
camera. Its validation now requires finite, positive data with the existing 0.8
cap, instead of assuming an unsupported physical-size minimum; wheel-hub and
basis geometry checks remain in force. No squared-radius interpretation is
assumed.

The subsequent 134-second production run passed the later entry guards and
emitted two Begin records for separate pause owners. Native images show a
rear-wheel close-up, pullback and slow pan; resume returned to the original chase
view and normal HUD, and another pause started a fresh transition. This is a
limited behavioral pass for the Mustang parked outside Dak's Garage, rather
than the interior upgrade/showroom camera. Menu panels obscure much of the car
in the wider shot. No frustum consumer during an active pause request was
observed, so moving-view culling beyond this position remains unqualified.
Both owned runs closed with empty Jobs and unchanged original 13-file profile
guards. Studio target production and fade binding remain unimplemented.

## Unresolved visual gates

The background envelope is intentionally **unbound**. A full-frame fade would
fade the car and UI, and skipping world draw calls without a scoped render-view
identity could break reflections, shadow passes, mirrors or another scene.
The existing renderer must supply a verified car-only layer or a separately owned
native car presentation before a blank/studio blend is implemented. No broad draw
suppression or shader/SDK modification is included.

The original native frontend renderer is a candidate, not an implemented
compositor: TU4 `CUI4PresentationRenderer` vtable `8204C814` delegates to its core
presentation at `+8`. Its view array is `[+0C,+10)`, records are `80` bytes, ID is
`+5C`, active byte `+79`. `827225C8` finds a view by ID, invokes core pre-render
when active, then runs `8271AC98` to publish its camera settings and render it.
View setup (`82738B18`, deferred wrapper `8271A408`) creates/finds a view and
copies a 24-byte viewport at record `+60`. The original viewport constructor
`824A0178` and setter `82425A50` identify its x/y/width/height/min-Z/max-Z fields;
this is **not a GPU-target descriptor**. Observing these records does not prove
target ownership, readiness or completion. Renderer virtual `+4C` (`82729468`)
separately contains offscreen resource/copy operations, which still require
ownership and lifetime analysis. Neither route establishes exclusive car
ownership or a transparent car layer. The core presentation still owns its world,
cars, lighting and deferred queue. Calling it with invented view IDs/resources or
globally switching to a homespace would be an unverified engine/state change.

Further source tracing narrows that candidate:

- `CPresentationViewDeferred::SetThumbNailPass` (`82E7ABF0`) queues the
  `822541FC` parameter type to callback `82E7ABD8`; that callback calls virtual
  `+5C`. The non-deferred implementation `82E93A50` writes only view byte `+1CE`.
  `CPresentationView::Render` (`824744D0`) reads that byte and packs it into bit 0
  of its per-view render flags (stack `+385`, around `8247497C`). This is a scoped
  view flag, **not evidence of car-only filtering or usable alpha**.
- In that same render function, view mask `+1D8` independently gates rendering
  groups. The bit-1 group at `82474E84..82474F10` iterates the core presentation's
  `[+50,+54)` collection and calls each object's virtual `+3C`; the unified car
  implementation `82E95790` forwards to its render object `+162C` and
  `82E59678`, which clears per-part flags `+1C/+1D`. That group is preparation,
  not proof of an isolated draw pass. Merely setting the thumbnail flag does
  not establish that only the user's car will be submitted. Other mask bits
  independently gate work in the render function; do not infer a car-only mask
  from the flag name.
- Original `CCarThumbnailManager` constructor `8254B798` initializes its shared
  renderer/car/camera references at `+68/+70/+74`. Its render state machine
  constructor `8254B838` registers four states from table `82012520`.
  Stage 2 (`8254ADD8`) accesses the manager-owned UI car `+70`, loads it through
  `8270B858` if needed, installs camera/view settings, updates its transform and
  calls the shared renderer. Stage 3 (`82526388`) begins asynchronous output
  through `82CF9B58`; its tick `8252D578` waits on completion. That is a thumbnail
  job with shared mutable resources and output lifetime, not a safe live-pan
  service to borrow during pause. This prototype never submits such a job.

The smallest reversible experiment for the next stage is therefore a **separately
owned native presentation view and target**, with a separately owned UI car if
the core cannot filter one existing render object. First prove an original
caller's view initialization, exact car selection, target readiness and deferred
release order; render a still car-only frame before attaching the pan or fade.
Then establish target clear/background behavior, culling, exposure, reflection
dependencies and ground/shadow ownership. Prefer matching car+studio RGB imagery:
fade WORLD+CAR to STUDIO+CAR behind the unchanged UI, with the same car pose,
camera and lighting in both images. This retains the car without assuming that
the native target supplies useful glass alpha; glass/AA/lighting ghosting still
needs image review. If an alpha-layer route is used instead, its transparency
and separate ground/shadow layer need independent proof. If any lifetime or
ownership check fails, retain the original scene and cancel the experiment.
No native view is allocated and none of these setters is invoked by this draft.

The next diagnostic is a private, read-only build delta: bounded observations of
frontend render `827225C8`, view/viewport setup `82738B18`, release `82734928`, and the
already hooked actual world-view reader `824744D0` (no duplicate hook). Each
wrapper retains exactly one original call and logs owner/view/camera/target
identities and native-view bridging across original child-screen/cancel paths.
Those observations do not identify a retained GPU target.
It must detect witnessed release/address reuse and distinguish queued render
work from a stale target. Actual target/draw captures are required alongside
those logs. Diagnostic scaffolding is kept outside this PR until qualified;
it allocates no native views and cannot establish studio rendering by itself.

Road geometry can still occlude the authored path. There is no proven collision
query or wall-clearance test here. Existing world lighting, reflections, exposure
and ground shadows remain in use; they are not studio lighting. A fresh active
camera now reserves the right side of Home, Cars and Settings for the shot,
using native FOV/aspect and a wheel-derived framing envelope. These pages place
controls on the left; Social retains its normal layout and cancels the camera.
Stale or rejected camera contexts restore the normal layout. The authored
envelope is not a native body bounding box or a wall-clearance guarantee.
Readability and framing still require actual image review during the moving shot.
Do not enable this experiment as a normal user feature before those gates pass.

## Validation and next runtime qualification

The standalone transition test exercises 30/60/120/144/240 Hz, rigid matrix
round-trips, all headings, invalid frames, interruption, cancellation and repeat
entry. This proves authored math, not guest camera behavior or visual quality.

The strict standalone test and all four affected TU4
translation units compiled successfully against the existing private R2 SDK;
TU0 and native-pause-disabled adapter builds also compiled. A small float easing
overshoot found by that test was fixed with double intermediates and clamping.
The native wheel-data revision passed the same focused checks, including geometry
selection, duplicate-corner rejection and upright-car checks. CI exposed the standalone VR hook fixture's missing link to
the out-of-line disabled adapter; disabled configurations now use inline no-ops.
That fixture compiles and executes for TU0 and TU4 locally, and all affected
active TU4 units still compile. Private diagnostic and production control hosts
have since linked successfully. The production control displayed world, native
pause and resume with the camera option off. With it on, the 144.407-second
production-host attempt also displayed those states but never started the camera
transition. Actual native chase and pause frames were captured; they provide no
zoom or pan acceptance. The production activation diagnostics compile and link
without the private presentation probes. Native target production and compositor
binding remain unimplemented; shader/reference tests do not qualify a studio image.

Runtime must use an exclusive Forza grant, current unlocked desktop, bounded
owned process Job/watchdog, copied profile and source/hash guards. Record original
driving view, approach, held close-up, pan, child-screen round trip, quick repeated
pause, and resumed view with option off as control. Confirm that the matrix-reader
hooks still run while paused, culling follows, and exterior body renders correctly.
No mockup or historical screenshot is evidence for this source version.

## Menu-aware framing result

At source `535897c`, both changed TU4 units, TU0/disabled adapter builds,
strict transition/projection/layout tests, TU0/TU4 VR fixtures and an ordinary
production link passed. Projection tests cover all headings in 15-degree steps,
30/60/120 Hz, three FOVs and three aspect ratios. The native FOV and viewport
are read only; framing translates the camera without changing projection.

A fresh 138.141-second isolated production run captured the rear-wheel close-up,
pullback, Home and Settings with the car clear of controls, Social restoring the
ordinary layout/view, and resume restoring the original chase view and normal
HUD. Cars was observed through Computer Use; its standalone native screenshot
request lagged input and did not save a Cars frame. Both camera entries emitted
Begin records. The owned Job emptied and all 13 original profile guards passed.

The same run also captured scenery covering almost the entire car during the
pan. Thus menu fit and resume have limited live evidence; an unobscured complete
pan does not. This was outside Dak's Garage, not interior upgrade validation.
An earlier attempt stopped presenting frames before camera activation; a fresh
retry ran successfully. The stall's cause is unassigned.

Studio fade remains unbound. See [the concrete provider boundary](pause-studio-provider.md)
for the missing native scene production and SDK image ownership/ordering work.

The production adapter now cancels and latches the shot at the end of pullback
(three seconds), restoring the original view and normal menu layout instead of
continuing into the known failing world pan. Resume/next pause clears the latch.
This temporary fallback does not qualify collision clearance for the earlier
approach/pullback and does not implement the requested studio pan or fade.
Its strict actual-adapter fixture verifies cancellation before a matrix write,
same-pause latching, cleared composition state, and re-entry after cancel. A
108.687-second live attempt again stalled presentation at the first cockpit
frame before any camera activation; it provides no live cutoff acceptance.

The next source adapter observes native camera/core/selection identities by
matching a fresh published pause matrix, including updates on the render thread.
Strict adapter tests cover cross-thread observation, ambiguous cameras/changed
selection, stale context, VR, cancellation and disabled operation. All three
affected TU4 units compile and the production host links. These observations
own no native objects and are not GPU readiness or a rendered studio scene.

## Recorded world experiment

The TU4 native-pause build now includes a default-off recorded producer and its
paired SDK consumer. `tools/build_rexglue_sdk.sh` applies the pinned SDK overlay
described in `patches/pause-camera-sdk/README.md`; the host and SDK changes must
be used together. This path observes the original world render and never creates
another scene, retains a borrowed guest view, or calls a duplicate world render.

The producer emits ordered frame, target and typed-owner records through the
original native allocator. A successful seal publishes its UID, captured revision
and source identity in one certificate. The consumer pins that exact record until
the real swap boundary, resets context and target witnesses at each replay end,
and stops immediately on lifecycle cancellation or changed source identity. Car,
UI, query, memory-export, target and freshness guards remain active. Native ring
rotation requires a bounded retained-span witness; unsupported metadata or slow
allocation stays rejected. The fixed rear-wheel hold does not advance the pan.

The diagnostic switches are `forza_pause_camera`, `forza_pause_provenance`,
`forza_pause_recorded_provenance`, `forza_pause_persistent_partial` and the typed
`forza_pause_{track,procedural,scene,crowd,particles}_binary` switches. All are off
by default. They are an experimental developer path, not a completed menu option.

Actual gameplay-to-pause-to-resume video has been captured. The car and native menu
remain visible and resume works, but complete scenery can still reappear: one
closed run had 17 rejected records among 111 completed pause-frame receipts,
all at the conservative native metadata witness. Persistent isolation is not
qualified. Progressive background fade and an unobscured complete pan remain
unimplemented; the old owned-studio provider is not part of this integration.

The SDK-free ownership and disabled/TU0 checks run with:

```sh
cmake -S forza-recomp/tests/pause-recorded -B build/pause-recorded
cmake --build build/pause-recorded --parallel 1
ctest --test-dir build/pause-recorded --output-on-failure
```
