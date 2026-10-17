# Built-in ultrawide gameplay and HUD layout

Enable **Settings > Display > Ultrawide gameplay**, then restart. The gameplay
camera follows the physical window aspect with a wider horizontal view. The
viewport query supplies both projection and culling, so the newly visible sides
do not rely on stretching the original camera or cropping its vertical view.
The feature is off by default and does not load external libraries or manifests.

Gameplay draws at the initial physical window resolution. At 3440x1440, the
main-view viewport and output image are 3440x1440; presentation is 1:1. Viewport
coordinates, screen-resolve sampling and generated pixel positions account for
the active dimensions. The tiled EDRAM/resolve storage retains an integer stride
(3840x1440 in this case), with unused columns outside the active drawing area.
It does not shade a 3840x2160 scene and shrink it to fit.

Reflection resolves have a separate square cube view. A 768x512 integer-stride
resolve is uploaded through a 2D array and filtered into 512x512 cube faces,
including the loaded mip levels. A rectangular image is never exposed as a cube
view. Layout barriers and cache memory accounting cover both images.

Native ultrawide currently supports physical heights divisible by 720, including
720, 1440 and 2160. **2560x1080 and other fractional vertical scales are not
supported.** At those sizes the host leaves native flags unset, disables camera
widening and logs the reason, retaining ordinary 16:9 rendering instead of the
broken banded output. This restriction is checked before changing the renderer
scales; HUD safe-area customization remains available separately. A fractional
vertical resolve fix is still needed before 1080-line ultrawide can be enabled.

Native ultrawide currently requires Vulkan host render targets. The launcher
selects Vulkan and disables manual render scaling while it is enabled. Windows
DPI scaling is included: 2752x1152 logical pixels at 125% gives 3440x1440 physical
pixels. Resizing updates framing; restart to change the drawing resolution.

## HUD controls

Enable **Customize HUD layout** to use the sliders at either 16:9 or ultrawide.
This can be enabled independently of camera widening. The default ultrawide HUD is centered in a 16:9 rectangle, retaining its proportions on a
wider display. **HUD safe width/height** fit that rectangle uniformly within a
smaller safe area; the smaller percentage controls the scale. This scales the
HUD as a group rather than stretching gauges independently along each axis.

**HUD horizontal/vertical position** move the group, with offsets clamped so the
rectangle remains on screen. Horizontal positive moves right; vertical positive
moves down. At 100% height there is no vertical space to move into: reduce the
safe area first. These are whole-HUD controls, not individual minimap/speedometer
anchors or an in-game drag editor. The launcher applies changes on the next run;
the corresponding engine cvars can also be adjusted through the SDK console.

The renderer transforms non-depth-tested packed front-buffer UI draws. World
geometry and depth-tested overlays are excluded. The map viewport is excluded
from camera widening; a recent map view or disappearance of gameplay projection
queries restores 16:9 presentation. The swap hook updates this state even while
gameplay timing is paused.

Judge HUD proportions using the final presentation image. Older guest-output
dumps used the padded storage aspect and could make text and gauges look
squashed. Native-mode output dumps now have the active 3440x1440 dimensions.

## Validation and current limits

The native-resolution path was built from this PR's public SDK patch series,
without DLSS, and tested on Windows/Vulkan with an RTX 5060 Ti. An isolated
3440x1440 run covered driving and entering the pause menu. RenderDoc captures
show the main-view viewport at 3440 pixels wide, final output at 3440x1440 and
square reflection faces. Separate testing covers inset/offset HUD controls.
Launcher, persistence and standalone C++ layout/state checks passed, including
square-widget proportions in final 2560x1440, 3440x1440 and 5120x1440 coordinates.

Full map-screen transitions, car-name overlays in races, split-screen, cutscenes,
and Linux runtime behavior still need testing. Pink highlights remain visible
on the car and road in the standalone NVIDIA gameplay test as well as some
paused backgrounds. Fixing square cube allocation does
not resolve every renderer artifact (issue #47); this remains experimental.
DLSS is now part of main through PR #60. Patch 0063 preserves its deferred
commands and fractional texture-coordinate correction, and the launcher retains
the DLSS settings locks when ultrawide is toggled. This does not establish
combined DLSS/ultrawide runtime support; that mode still needs separate validation.

For the standalone math/state test, compile and run
`forza-recomp/tests/ultrawide_math_test.cpp` with a C++17 compiler. Launcher tests
run with `python -m unittest launcher/test_ultrawide.py`.

**Expand HUD to ultrawide edges** anchors supported side HUD draws outward while
retaining the same native widget scale. At 3440x1440 and 100% safe area, each side
moves 440 pixels outward from the centered 16:9 layout. The original game insets
remain; widgets do not touch the last screen pixel. Safe-area controls still
reduce the occupied area, and group offsets are clamped against the wider bounds.
Menus retain protected presentation. SDK patch 0063 recognizes the game's affine
HUD/font shader families and translates whole draws based on their origins;
unknown families and the central origin band retain centered placement. This is
experimental: large centered text, race overlays and projected labels still need
broader coverage before treating it as a complete per-widget anchoring system.

PR #59 was split at the maintainer's request. Patch 0063 contains only native
ultrawide rasterization, square reflection faces and proportional/edge-anchored
HUD placement. No NVIDIA runtime, reconstruction controls or general readback
changes are required. Earlier screenshots came from the combined development
build; the standalone split was rebuilt and checked separately at 3440x1440.

Pinyon Shift attribution: `src/platform/ultrawide.cpp` and
`src/hooks/ultrawide_projection.cpp` in forza-recomp carry the adapted view-state
and projection strategy. In patch 0063, the presentation cvars and HUD viewport
transforms in `src/graphics/flags.cpp`, the D3D12/Vulkan command processors and
`src/ui/presenter.cpp` carry adapted presentation logic. The original BSD-3-Clause
notice is retained in both the game source/package and the SDK patch directory.

Standalone split validation (2026-10-16): the complete 47-patch stack applies to
bd833a2, the Windows GPU plugin builds with Vulkan and D3D12 enabled, and the
TU4 game builds from the native-only source. The C++ layout/state checks and
three launcher persistence/bounds tests pass. An offscreen Settings-page check
confirms Vulkan/automatic-scale dependencies and native HUD sliders, with no
DLSS controls. Fresh standalone gameplay and launcher captures are attached to the PR rather
than Git. The game reached free roam, moved the car and entered the pause menu
at an actual 3440x1440 output, with DLSS absent. This is a functional smoke test,
not a performance benchmark or a claim that the pink highlights are fixed.
