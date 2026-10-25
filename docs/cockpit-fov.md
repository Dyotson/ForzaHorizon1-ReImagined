# Cockpit field of view

The launcher's Display > Camera setting and TU4's native Graphics screen expose
**Cockpit FOV (vertical)**. Stock (`forza_cockpit_fov = 0`, the default) leaves
the game's camera state untouched, normally 62 vertical degrees. Explicit
overrides accept integer degrees from 40 to 90. Stock is also the reset choice.
The bounds avoid extreme narrow views and near-180-degree projection distortion;
they are a supported setting range, not a claim that all car interiors suit every
value. Native Save applies and persists; Back discards unsaved edits. The launcher
imports the native settings file before the next launch. The macOS app writes its
external TOML on Save through the existing graphics configuration path.

Only the active `Camera::CCamDriver` is eligible. The separate `CCamCockpit`,
chase, hood, bumper, photo, menu and rear-view camera classes are excluded.
The current world's existing pause/replay flag, loaded flag and time scale also
exclude replays, rewinds and paused/loading worlds, even with a driver camera.
Mirrors retain their own rear-view projection. The override does not change the
camera's position, near/far planes, aspect ratio or its simulation state.
Ultrawide continues to expand the horizontal view using the selected vertical
FOV. The setting is bypassed while `vr_head_pose_test` is enabled, including an
identity pose. There is currently no headset stereo projection implementation;
a future VR projection owner must retain this bypass and supply its own per-eye
frustum rather than use this flat-screen setting.

## Runtime ownership and hook points

The director's `+304` is vertical FOV in radians. The driver vtable is
`0x82075DD4` (TU4 `0x820860FC`). Stock camera settings identify Driver FOV as
62 vertical degrees; see [the camera research](vr.md). The implementation uses
the two existing head-pose hook boundaries, without an SDK projection patch:

| Reader | TU0 | TU4 | Consumer |
| --- | --- | --- | --- |
| `CSplitView` presentation update | `0x825A9D20` | `0x82621F20` | Presentation FOV setter, then complete deferred camera update |
| View 0 visibility frustum | `0x829A4860` | `0x82A58DA0` | Game-side view matrix/FOV/aspect/clip frustum builder |
| Director FOV accessor | `0x8284EF90` | `0x828FF260` | Loads one float from director `+304` |

TU4 generated-source inspection confirms both readers call `0x828FF260`.
Presentation passes its result through the original unit conversion to the
camera's virtual setter at `+56`, and reads it again for the complete deferred
update. Visibility applies the same conversion for `0x8241AF90` (TU0
`0x823EAED0`). Both calls therefore see the same overridden vertical FOV;
render-side projection/culling and game-side visibility retain their existing
camera pipeline. Aspect stays in the existing viewport query, including the
ultrawide hook. Rear-view uses a different camera vtable and is excluded.

Each reader temporarily substitutes the big-endian float and restores the exact
original bytes afterward, unless the guest wrote a newer value. Default zero
does not substitute any bytes. Camera transitions to a different active class
stop the override immediately. No archives, game settings files or game data
are modified or distributed.

The existing world-update hook `0x823E40D8` (TU4 `0x82414068`) records its
world address before calling the original. TU4's original calls the camera
world update `0x824131A8` within that call. Readers inspect the current world's
`+0x0C` pause/replay flag, `+0x0D` loaded flag and positive float time scale at
`+0x28`, the same fields documented and used in `platform/driving_gate.h`.
This uses no frame-delay or rumble heuristic. A world that has not been observed
is ineligible.

## Validation

`cockpit_fov_test.cpp` checks every supported degree, radians conversion,
identical values for both reader scopes, exact restoration, adjacent bytes,
stock/invalid values, excluded cameras, VR bypass and preservation of a newer
guest write. Launcher tests check native Save import, invalid-request atomicity,
TOML output, persistence across reload and stock reset. Both TU0 and TU4 hook
translation units are syntax checked against the exact 121-patch SDK.

Windows TU4 in-game checks on an RTX 5060 Ti (Vulkan driver `0x9A438000`)
used an offline, disposable copied profile at the
same parked VW spawn, 1280x720 window, 60 FPS cap and native Vulkan rendering.
Stock versus 90 degrees visibly changes the cockpit's vertical framing while
chase and main-menu framing remain unchanged. Both mirror images render.
The native Graphics screen was checked at the 1080p output preset: an unsaved
40-degree edit wrote no settings file, Back restored Stock after reopening,
and Save wrote 40. The launcher imported that actual file, generated TOML with
40, and a restarted game displayed 40 in the native menu and the narrower
cockpit view. The shorter native Stock value fits between the spinner arrows.

With 90 selected, the native rewind menu displayed stock cockpit framing.
The observed menu's X / View Replay entered the game's overhead replay camera
and native replay toolbar successfully. This is a replay smoke test; every
replay camera has not been compared against stock.
With 90 and `vr_head_pose_test=true`, an identity pose also retained stock
framing; the log confirmed zero rotation/translation and the enabled prototype.
This validates the prototype bypass, not real-headset stereo projection.
Every run had a hard time limit, closed its owned process, and verified the
source profile hashes remained unchanged. Captures, copied saves, caches and
build products remain outside Git.

The Windows host compiled all 83 current authored sources and linked normally
against immutable TU4 generated objects. The SDK's 1,265-file SHA-256 manifest
and 121-patch manifest matched. After the paired camera runs, only the native
Stock label changed in runtime sources; the camera sources were unchanged.
A full TU0 host build/game run, real-headset VR, other cars, continuous driving
at the bounds and an ultrawide in-game FOV comparison remain unverified.
