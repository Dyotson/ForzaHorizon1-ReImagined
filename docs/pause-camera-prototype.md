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
`RenderViewToTexture` (`82738B18`, deferred wrapper `8271A408`) can create/find a
view and attach a target description at record `+60`. This establishes a native
offscreen-view route worth pursuing; it does **not** establish exclusive car
ownership or a transparent car layer. The core presentation still owns its world,
cars, lighting and deferred queue. Calling it with invented view IDs/resources or
globally switching to a homespace would be an unverified engine/state change.

Road geometry can still occlude the authored path. There is no proven collision
query or wall-clearance test here. Existing world lighting, reflections, exposure
and ground shadows remain in use; they are not studio lighting. Menu geometry and
readability are unchanged and require actual image review during the moving shot.
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
selection, duplicate-corner rejection and upright-car checks. No full host link
has been attempted. The exclusive runtime queue and locked desktop have prevented
any game session or fresh screenshot for this prototype so far.

Runtime must use an exclusive Forza grant, current unlocked desktop, bounded
owned process Job/watchdog, copied profile and source/hash guards. Record original
driving view, approach, held close-up, pan, child-screen round trip, quick repeated
pause, and resumed view with option off as control. Confirm that the matrix-reader
hooks still run while paused, culling follows, and exterior body renders correctly.
No mockup or historical screenshot is evidence for this source version.
