# macOS experiment

This is the first experimental macOS version, for Apple Silicon. Both base and TU4 opening
drives render and respond to driving input. Patch 0080 fixes the black world;
patch 0082 fixes the tested Viper material crosshatch with normal mip filtering.
The Python launcher still targets Linux and Windows; use the scripts below.
Full races, save reloads and physical controllers still need validation.

The macOS work is based on the performance-overlay branch (#133), including its
renderer, anti-aliasing and in-game graphics-menu parents. The seven macOS SDK
patches are numbered 0079–0085 after that shared series. Both game variants must
be rebuilt against the combined SDK; do not mix an older app executable with
the new runtime libraries.

## Prerequisites

- Apple Silicon Mac running macOS, with Xcode and its command-line tools.
- CMake 3.25+, Ninja, Git, and Python 3 (`brew install cmake ninja`).
- Your own base-game files, including `default.xex`, `XMediaFacade_default.xex`,
  `SpeechFacade_default.xex`, and the media files. See [disc extraction](disc.md).

All commands run from the repository root. Game data, SDK source, compiled output,
and the test profiles are ignored by Git.

## Build

```sh
# Build the pinned SDK and apply this repository's complete patch series.
# First run downloads the SDK source and dependencies and compiles MoltenVK too.
tools/build_macos.sh --sdk-only

# These checks do not need game files. They do not link or boot Forza.
tools/build_macos.sh --host-check
tools/build_macos.sh --host-check --tu4

# Load the native Xenos plugin and create a Vulkan device through the SDK.
# This is headless: it does not test window creation, guest commands or rendering.
tools/build_macos.sh --gpu-check

# After extracting the base game into extracted/game:
tools/build_macos.sh --game-only
tools/play_macos.sh
```

`tools/build_macos.sh` without a mode builds both SDK and base game. The SDK is
installed at `forza-recomp/out/macos-sdk`; `FORZA_MACOS_SDK` overrides that prefix.
`REXSDK_SRC` overrides the SDK source directory (default
`forza-recomp/out/macos-sdk-src`); use a dedicated checkout, since the canonical
patch script checks out the pinned commit and replaces the patched files.
`REXSDK_JOBS` sets build concurrency (default 6).

For Title Update 4, first follow [the update instructions](dlc.md) to create
`extracted/tu4`, then use `--game-only --tu4` and `tools/play_macos.sh --tu4`.
Base and TU4 builds have separate build directories and test profiles.

The macOS play script defaults to offline, 30 fps and the console's 1280×720
render resolution. It uses
`forza-recomp/out/macos-profile-0` (or `macos-profile-4`) for saves, logs and cache,
and does not import existing saves. Set `FORZA_DATA` for an alternate base media
folder, `FORZA_UPDATE` for alternate TU4 files, and `FORZA_MACOS_PROFILE` for an
alternate test profile. Avoid pointing that variable at your normal profile.
Normal mipmaps and anisotropic filtering are enabled. Following Pinyon Shift's
macOS tuning, the script defaults `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` to `0`;
an explicitly supplied environment value takes precedence.

For the tested lower-latency configuration:

```sh
tools/play_macos.sh --tu4 --skip-intro --low-latency
```

This requests 60 fps, limits the GPU to one queued frame and allows IMMEDIATE
presentation (which can tear). The opening drive measured roughly 45–50 fps at
720p on this M4 Pro; this is frame throughput, not a measured input-to-display
latency. Caller-supplied flags override
the preset. The script avoids duplicate frame-rate arguments: CLI11 rejected
the previous fixed `30` plus a caller's `60`, leaving the higher-rate experiment
invalid. That argument handling is now fixed.

Retina rendering was also tested: `--resolution_scale=2` produces 2560×1440,
and `--resolution_scale=3` produces 3840×2160. GPU capture headers confirm both.
The 4K opening scene measured about 12–15 fps; 1440p with the lower-latency
settings measured about 26–27 fps. Both are too slow for responsive driving
here. The screen is 3024×1964; these are whole-number render scales presented
on that display. Exact pixel-matched drawing at its fractional scale remains
unsupported by this renderer's vertical resolve path.

## Local macOS app

```sh
tools/package_macos.sh --tu4
open "forza-recomp/out/app/Forza Horizon TU4 responsive.app"
# More throughput with two queued GPU frames:
tools/package_macos.sh --tu4 --balanced
# Higher ceiling and cheaper Retina presentation:
tools/package_macos.sh --tu4 --max-fps
# Optional sharper reconstruction, with a higher rendering cost:
tools/package_macos.sh --tu4 --fsr-1440p
```

The helper creates an ad hoc signed app from the current build. It sets the
macOS game category and Game Mode support keys, and uses native fullscreen.
Patch 0083 raises the visible window before requesting fullscreen. Game Mode
activation at launch was confirmed in the macOS GamePolicy log, but it also
paused between later observations; sustained activation is not established for
every performance sample. See [Apple's Game Mode requirements](https://support.apple.com/en-us/105118).
The app's minimum macOS version is taken from the compiled executable.

The responsive preset requests 60 fps, one queued GPU frame, IMMEDIATE
presentation, 720p rendering and FSR 1 spatial presentation sharpening. The
`--balanced` preset keeps those settings with two queued frames. A settled
25-second TU4 opening-scene window measured 59.97 guest fps (59.87 presented),
with a 17.72 ms guest p95 (17.97 ms presented). The one-frame preset measured
56.23 guest fps (56.21 presented), with a 19.35 ms guest p95 (24.21 ms presented).
Both used the packaged app, a 60 fps ceiling and Game Mode. Initial shader
compilation and traffic can still cause hitches; this is not a full-race result.
The extra queued frame can add input latency. The CLI equivalent is
`tools/play_macos.sh --tu4 --skip-intro --balanced`. The
`--fsr-1440p` preset uses the existing FSR 3.1.4 path: 720p world reconstruction
with a 1440p HUD. This measured about 33–34 fps in the opening scene, compared
with about 29 fps for the earlier direct 1440p run. It does not render the
world at native 1440p. `--1440p` selects direct 1440p rendering instead.

Each preset has a separate app profile in `forza-recomp/out/macos-app-profile-*`.
Saves and `forza.toml` remain outside the signed bundle. Existing settings are
preserved; the preset initializes a new profile only. `FORZA_MACOS_APP` and
`FORZA_MACOS_PROFILE` override these paths. The app includes the recompiled
game and local media paths; keep it local. The helper refuses to replace an
existing bundle. It uses the shortened startup-movie clone if already prepared.

The latest direct app launch needed scripted controller input to advance a black
startup screen; keyboard input did not advance it in that check. Physical
controller startup remains unvalidated. For the tested menu-advance sequence:

```sh
open -n "forza-recomp/out/app/Forza Horizon TU4 responsive.app" \
  --env 'FORZA_AUTOPRESS=3:A,7:START,11:A,14:A,18:A,22:A'
```

This advances the initial menus once and leaves the game running. The CLI's
`--test-drive` option uses the same sequence.

TU4's **Graphics** page includes MetalFX Temporal and Spatial choices, native
720p output, and the FPS/frame-time graph. Edits apply together with **A / Save**;
**B / Back** discards pending edits. Saving also updates the Mac profile's
external `forza.toml`, so a direct app launch retains the choices without the
Python launcher. MetalFX currently accepts 1440p and 4K output; other requests
report an error and restore the preceding selection. The disc build retains its
original menu.

## Maximum frame rate

```sh
tools/play_macos.sh --tu4 --test-drive --max-fps
# Or launch the separate local app/profile:
open "forza-recomp/out/app/Forza Horizon TU4 max-fps.app"
```

This preset keeps the 1280×720 world and HUD, the game's MSAA/effects, normal
mipmaps and texture filtering. It requests a 240 fps ceiling, a 480 Hz guest
vblank clock, two queued GPU frames and IMMEDIATE presentation. It displays an
FPS counter. The ceiling is a pacing setting; it does not mean this Mac renders
240 frames per second. The earlier responsive and balanced presets remain
available with their 60 fps ceilings.

Retina presentation uses one FSR 1 EASU pass to 2560×1440, sharpening at that
size, then the final stretch to the drawable. The previous full FSR chain also
ran another EASU pass for the remaining small stretch. This change affects final
presentation, not the game's scene rendering. Override `--present_effect` or
`--present_fsr_max_upsampling_passes` on the play command to compare. An explicit
frame-rate, queue-depth, refresh-rate or overlay argument takes precedence.

TU4 stationary opening-drive samples on the M4 Pro (seconds 130–155):

| Settings | Average guest FPS | Guest p95 |
| --- | ---: | ---: |
| Previous balanced preset, 60 fps ceiling | 59.3 | 18.5 ms |
| Original FSR chain, 120 fps ceiling, two queued frames | 65.9 | 15.9 ms |
| One-pass FSR, 240 fps ceiling, two queued frames | 66.5 | 16.2 ms |
| Bilinear presentation, 240 fps ceiling, two queued frames | 64.8 | 16.9 ms |
| Original FSR chain, three queued frames | 64.2 | 17.3 ms |

These are opening-scene throughput samples, not full-race or input-latency
measurements. Concurrent Simulator/media-analysis work varied during this set;
the small differences between uncapped configurations do not establish a
general performance ranking. Removing the previous 60 fps ceiling permits the
observed mid-60s throughput. Bilinear did not establish an advantage, so the
preset retains sharpening.

An Apple direct-to-image texture-upload experiment and a SPIR-V optimization
pass were tested and removed after they failed to show consistent throughput
improvements. Shader optimization passed offline Vulkan/scalar-layout checks
and rendered a correct world/HUD, but alternating on/off windows varied in both
directions. The rejected patches, commands, per-frame CSVs, captures and process
load samples remain in
`forza-recomp/out/macos-validation/fps-max/20261018-054219`.

## 1440p at 60 fps target

Native 1440p means a 2560×1440 world and HUD. The earlier max-FPS preset renders
both at 720p and sharpens the finished image for Retina presentation; its
mid-60s throughput does not meet a native 1440p/60 target.

Fresh TU4 opening-scene measurements on the M4 Pro:

| Mode | World raster | HUD | Average guest FPS |
| --- | --- | --- | ---: |
| Native 1440p | 2560×1440 | 2560×1440 | 30.2 |
| MetalFX Temporal, original copy | 1280×720 | 2560×1440 | 37.2 |
| MetalFX Spatial, original copy | 1280×720 | 2560×1440 | 40.4–41.8 |
| MetalFX Spatial, packed-word copy (0085) | 1280×720 | 2560×1440 | 49.4–54.8 |
| MetalFX Spatial, packed-word copy, VSync off | 1280×720 | 2560×1440 | 59.1 |

The Spatial rows alternate the copy algorithms in the same game session,
excluding the transition periods. Patch 0085 removes repeated byte extraction
and division from the cross-scale resolve mirror. It preserves the original
pixel mapping, with 270 format/scale/alignment reference checks and clean
world/HUD/solid-stripe captures. `--fh1_fast_resolve_copy=false` restores the
original algorithm for comparison. Native rendering without split caches does
not use this mirror and does not gain FPS from this change.

These are background throughput measurements. The foreground app was
`loginwindow`, GamePolicy reported Forza as not visible, and media analysis
competed during the samples. The frame-time tails still contain stalls. An
unlocked, foreground driving test is required before claiming steady 60 FPS;
neither native nor reconstructed 1440p/60 is verified by this set.

After unlocking the Mac, fresh fullscreen runs measured
26.66 fps for native 1440p and 53.84 fps for MetalFX Spatial (720p world,
1440p HUD/output). Both used VSync off and two frames in flight. The Spatial
run's guest/present p95 was 19.84/20.13 ms. World, HUD and solid stripes were
visually checked in the foreground. Simulator rendering remained active;
these are concurrent-workload opening-scene results, and steady 60 fps is
still unachieved. The earlier 59.1 fps background result is not a foreground
gameplay guarantee.

After Simulator was shut down, the same Spatial preset averaged 51.79 guest /
51.79 presented fps. Median guest time was 18.27 ms, p95 20.23 ms, and the run
included a 1.16-second stall. A vector-store copy experiment showed no material
gain over 0085 and was removed. Bilinear final presentation also failed to
establish a reliable improvement. High Power mode on AC and the final window
focus adjustment have not yet received a settled performance measurement.

The VSync-off sample averaged 59.12 guest / 59.08 presented fps, with guest
p95 of 18.0 ms and present p95 of 25.0 ms. It still contains stalls. The retained
performance preset uses that configuration, preserving the normal game MSAA,
mipmaps, filtering and effects. VSync off also changes the SDK's guest vblank
clock to 1000 Hz and releases its GPU wait pacing. This is an uncapped mode;
the stored `frame_rate=240` does not enforce a strict 240 fps ceiling here.
Frame-time logs matched elapsed game time, and a throttle capture reached 17 mph
with a clean world, HUD and solid stripes. Input-to-display latency is unmeasured.

A native-1440p Metal trace found fragment work to be the largest GPU channel.
The shader optimization trial remained around 31 fps and was removed. Its old
experimental cache bit also conflicts with the fractional-raster bit and must
not be reused unchanged. No new shader precision or MSAA changes are retained.

```sh
# Entire world and HUD rendered at native 1440p:
tools/play_macos.sh --tu4 --test-drive --max-fps \
  --resolution=1440p --resolution_scale=2 \
  --fh1_upscaler=off --fh1_scene_720p_native_hud=false

# Reconstructed 1440p, with a native 1440p HUD:
tools/play_macos.sh --tu4 --test-drive --1440p-performance
tools/package_macos.sh --tu4 --1440p-performance
open "forza-recomp/out/app/Forza Horizon TU4 metalfx-performance.app"
```

Per-frame data, paired results, configuration and captures are retained under
`forza-recomp/out/macos-validation/1440p60/20261018-121435`.

## Apple MetalFX

Patch 0084 adds native MetalFX temporal and spatial reconstruction. The tested
configuration renders the world at 1280×720, reconstructs it to 2560×1440, then
draws the HUD at 1440p. This improves output detail without shading the world at
1440p. It is experimental and does not currently achieve a stable 60 fps here.

```sh
tools/play_macos.sh --tu4 --skip-intro --metalfx
# Spatial alternative, without temporal history or projection jitter:
tools/play_macos.sh --tu4 --skip-intro --metalfx --fh1_upscaler=metalfx_spatial

tools/package_macos.sh --tu4 --metalfx
open "forza-recomp/out/app/Forza Horizon TU4 metalfx.app"
# Optional separate spatial app/profile:
tools/package_macos.sh --tu4 --metalfx-spatial
```

The MetalFX preset requests a 120 fps ceiling, IMMEDIATE presentation and two
queued GPU frames. The higher guest vblank rate gives finer pacing when the
renderer cannot keep up; it does not imply that 120 fps is achievable here.
One queued frame was slower; two permit more overlap but can add input latency.
Use `--vulkan_max_frames_in_flight=1` to compare. The default responsive preset
retains 720p rendering and one queued frame. Existing app bundles and profiles
are preserved, so rebuild into a new `FORZA_MACOS_APP` path when necessary.

Vulkan and Metal share the upscaler textures through `VK_EXT_metal_objects`.
Two GPU timeline events order scene preparation, native reconstruction and HUD
composition. Format conversion stays on the GPU; there is no CPU image readback
or `waitUntilCompleted` in the normal frame path. Temporal MetalFX uses the
existing packed-depth decoder, per-object motion reconstruction and Halton
projection jitter. Spatial MetalFX disables jitter and history. Unsupported
initialization reports an error and falls back to native rendering.

Opening-drive captures verified the world, HUD, solid Viper stripes and response
to throttle/steering. The packaged temporal app also activates Game Mode,
confirmed in the GamePolicy log, and exits through its native Quit menu.
Settled 25-second TU4 windows on the M4 Pro, with the 120 fps measurement cap,
720p world, 1440p HUD and two queued frames:

| Reconstruction | Average fps | Guest frame time p95 |
| --- | ---: | ---: |
| MetalFX temporal | 38.4 | 27.2 ms |
| MetalFX spatial | 40.7 | 25.8 ms |
| FSR 3 Performance | 39.6 | 26.4 ms |

Temporal was measured in the packaged app with Game Mode; the other runs used
the CLI. Traffic and camera positions vary slightly. These small differences
do not establish a general performance advantage. The initial temporal run
with one queued frame measured about 33 fps. These are opening-scene throughput
measurements, not full-race or input-to-display latency results. Logs, captures
and `final-comparison.json` are under
`forza-recomp/out/macos-validation/metalfx-integration`.

The integration runs at the game's existing composition point after tone
mapping and postprocessing. Those effects are already present in the input;
it is not a new HDR render pipeline. Temporal image quality still needs broader
motion, transparency and disocclusion testing. The main Metal command-buffer
interval excludes MetalFX's internal command buffers and is not the total
upscaler cost. Instruments shows substantial compute work in the surrounding
renderer, so replacing the upscaler alone does not remove the main bottleneck.

A single-sample MSAA experiment increased throughput but corrupted reflections
and vegetation. It was removed. A 1080p output trial cropped the HUD rather than
reducing its backing raster; MetalFX rejects that fractional output. Exact
3024×1701 display-matched rendering still needs a renderer change. Keep normal
MSAA, mipmaps and anisotropic filtering for the retained presets.

## Portability changes

- The Linux x86-64 CPU sampling profiler is disabled on unsupported hosts; use
  Instruments for macOS profiling.
- RenderDoc capture is disabled on macOS. The existing guest dump and host screenshot
  code is validated here through GPU-generated PPM captures.
- The Linux-only fiber interposition tracer is excluded on macOS.
- GPU waits keep `nanosleep` on macOS, with the Linux timer-slack call guarded.
- CMake recognizes `.dylib` GPU plugins. The macOS build script installs codegen,
  runtime, plugin and headers from the same patched SDK build.
- Guest DLLs use the bare filenames the SDK's POSIX module registry requests.
- SDK patch 0079 iterates the existing function mapping table, avoiding the
  77,344-call registration routine that made AppleClang ARM64 compilation very slow.
- SDK patch 0080 fixes rectangle shader control flow on GPUs without geometry
  shaders, restoring the final scene composition on Apple Silicon.
- SDK patch 0082 allows contraction in Apple color shaders while retaining
  precise vertex/depth math, removing the tested material crosshatch.
- SDK patch 0083 enters Cocoa fullscreen after showing the window, allowing
  the packaged game to activate macOS Game Mode on launch.
- SDK patch 0084 adds native MetalFX reconstruction with GPU event interop.
  Both base and TU4 opening drives render with the temporal path; TU4 also
  validates spatial reconstruction and the separate responsive/balanced presets.

## Validation on 2026-10-17–18

Host: Apple M4 Pro, macOS 27.0.1, AppleClang 21, Release arm64. SDK source:
`bd833a2ab69d305f99ddf57418fc38ffec8af93d` plus the repository's patch series.

After stacking on #133 (`03220d1`), the complete patch series through 0084
applied cleanly and matched the SDK source used for the rebuild. Both base and
TU4 game builds, host compilation checks, native Vulkan device creation,
standalone preset/monitor/config tests and launcher import tests passed (the
Qt-dependent import test was skipped because Qt was unavailable).

TU4 runtime checks covered native 720p, live switches to both MetalFX modes,
rejection of unsupported MetalFX 1080p output, Back discarding an unsaved edit,
and restart persistence of MetalFX plus the frame-time graph. Switching back to
native 720p also succeeded. The 130–155 second opening-drive sample at 720p,
with a 60 fps ceiling, two queued frames and the graph enabled, averaged 59.33
guest fps / 59.29 presented fps; p95 was 18.46 / 18.35 ms. It included one
278 ms guest hitch. The rebuilt base app also reached the opening drive,
responded to throttle (18 mph capture) and retained solid stripes and a clean
HUD. Earlier performance tables above describe the preceding
SDK build, not this combined branch. Evidence and rollback copies are under
`forza-recomp/out/macos-validation/rebase-performance-parent/20261018-050807`.

- Native SDK runtime, code generator and Xenos plugin built successfully.
- A complete SDK install, including Vulkan loader and MoltenVK, succeeded.
- Host source compilation passed for the base and TU4 configurations, including
  `forza_app.h`, without generated game code.
- The native Xenos plugin loaded and the SDK created an emulation-capable Vulkan
  1.3 device on the M4 Pro through MoltenVK. Fragment shader interlock was enabled.

With the user's base game (0.0.0.10, media ID `2DC7007B`) and TU4:

- Extracted the base package and applied TU4: all 371 executable page groups and
  all 61/54 facade page groups matched the update's digests.
- Generated and linked native arm64 Release executables for the base game and TU4.
- Both builds opened a native SDL/Metal-backed window and reached their menus.
  TU4's menu includes Horizon Rally; the DLC itself was not supplied or tested.
- Controller injection advanced the title screen, initial video setup, single-player
  menu and intro video. In-game captures confirm those screens render.
- Before patch 0080, both builds reached the opening drive with a black world and
  visible HUD. Verified interlock rendering also showed a black world.
- After patch 0080, both builds render the road, terrain, trees, cars, cockpit,
  mirror and HUD. TU4 was retested with a clean SDK build without diagnostics. Scripted throttle and steering move the car; a capture shows
  20 mph and changed road position.
- Before patch 0082, normal filtering produces vehicle/cockpit crosshatch
  artifacts. The base-mip fallback in 0081 hides these at the cost of distant
  texture shimmer. The original path logged roughly 17–24 fps.
- A TU4 on/off comparison of pixel shader contraction reproduces broken stripes
  at roughly 21–24 fps with contraction off, and solid hood/rear stripes plus a
  clean cockpit with it on. Normal mipmaps and anisotropy are retained. The
  packaged patch 0082 was retested with precise vertex/depth math; a driving
  capture shows 55 mph and solid stripes. It reaches the configured 30 fps cap;
  this is not an uncapped performance measurement.
- After rejecting the memory backport described below, both base and TU4 were
  rechecked with the restored 0082 SDK: solid stripes, normal filtering and a
  clean speedometer/minimap. Host checks and native GPU creation passed too.
- Audio endpoint initialization succeeded; audio quality has not been assessed.

Captures and logs are local in `forza-recomp/out/macos-validation`. Frame rates are
from this opening-drive experiment, not a full-game performance result. Full races,
save round trips and physical controller behavior still need validation.

## Black world: cause and fix

GPU readbacks showed valid scene depth and a colored world texture entering the
post-processing passes. Forcing the tone-mapping pixel shader to solid magenta still
produced no visible scene. Its rectangle vertex shader contains branches; Apple's
GPU does not expose Vulkan geometry shaders, so the SDK evaluates each rectangle's
three source vertices in a loop and expands them into a triangle strip.

That fallback created its next-program-counter `OpPhi` before emitting the path
that starts the next source vertex. The path then appended its incoming operands
too late. Patch 0080 defers filling that Phi until every predecessor is known.
The colored scene is restored with normal color grading, normal shader math and
the default host-render-target path. Temporary buffer capture, LUT bypass and
solid-color diagnostics are not part of the shipping patch.

## Fast rendering tests without cutscenes

```sh
brew install ffmpeg # only needed for the test-data helper
tools/play_macos.sh --skip-intro # interactive, without startup movies
tools/play_macos.sh --test-drive # also navigate to Single Player
# or tools/play_macos.sh --tu4 --test-drive
```

The first test launch makes an APFS clone in `extracted/macos-test-game`, then
stream-copies only the opening frames of the five startup movies. Original game
files are untouched. It navigates the isolated profile to Single Player with
scripted input. World loading and shader compilation still take time.
`FORZA_MACOS_TEST_GAME` overrides the clone destination. Normal `tools/play_macos.sh`
continues to use the original media.

## Vehicle stripes: shader fix and diagnostic fallback

The Viper's stripe texture contains solid black stripes. GPU readback confirmed
that all eleven mip levels contain the expected artwork. A material-only diagnostic
still showed the crosshatch, whereas forcing mip 0 restored solid stripes.
Automatic 2D mip selection did not remove the problem. Comparing the same guest
material shaders with Pinyon Shift led to its pixel-math tuning. Permitting
contraction removes the tested artifacts without changing the texture data or
samplers. The exact underlying Metal compiler defect remains unknown.

Patch 0082 enables `spirv_pixel_shader_contraction` on Apple GPUs. It retains
the existing SM3 zero/NaN rules and keeps vertex shaders, depth-writing shaders,
memory exports and synthetic depth-only shaders precise. Restart with
`--spirv_pixel_shader_contraction=false` to compare. Strict and contracted
variants have distinct shader/pipeline keys.

Patch 0081 still provides the optional `vulkan_force_base_mip` diagnostic.
It clamps 2D samplers to mip 0 and disables anisotropy. Distant textures can
shimmer more. It is off by default; pass `--vulkan_force_base_mip=true` to use it.

## Pinyon Shift comparison

[Pinyon Shift](https://github.com/arcanite24/pinyon-shift) has a dedicated FH1
native renderer and documented Apple Silicon support. Adopting its full renderer
would require integration with this project's older SDK ABI and title hooks.
We built its base and TU4 executables separately from verified local game data,
using source revision `9331e45` and ShiftGlue `79d3e6f`. Original game files and
existing saves were not modified.

Both fresh-profile opening-drive render tests passed, with solid stripes,
normal mipmaps, 4x MSAA, bloom, motion blur and depth of field enabled. Median
frame time over the final 1,800 output frames was 18.55 ms (53.9 fps) for base
and 19.28 ms (51.9 fps) for TU4, at 1280×720 on this M4 Pro. These measurements
cover the opening scene, not a complete race or a general 120 fps claim.

The reference SDK includes a local pipeline-cache snapshot lock fix: the first
run hit `VK_INCOMPLETE` because compilation could grow the cache between its
size query and data read. A shared lock lets pipeline creation remain parallel;
an exclusive lock covers both snapshot calls. Both full-effects reference tests
then passed their strict log checks. The patch and logs are saved locally under
`forza-recomp/out/macos-validation/upstream`.

### Fresh frame-rate comparison on October 18

Both TU4 builds were run sequentially at a 120 fps cap, fullscreen at the same
3024×1898 drawable size, with IMMEDIATE presentation, three queued GPU frames,
normal mipmaps, 8x anisotropy, 4x MSAA, bloom, motion blur and depth of field.
Both used FSR 1 spatial presentation; the 1440p world was rendered directly,
without temporal reconstruction. Pinyon's native FH1 renderer and this project's
Xenos renderer remain different implementations.

| Internal resolution | This port, average fps | Pinyon, average fps |
| --- | ---: | ---: |
| 1280×720 | 63.6 | 52.4; 56.3 on repeat |
| 2560×1440 | 30.3 | 37.9 |

Each measurement covers 25 seconds after the opening-drive camera settles,
with the Viper stationary beside the road. Traffic timing and small differences
in car/camera position remain. Main-port windows were seconds 130–155; Pinyon
used the final 25 seconds of each route. Source and presentation rates agreed.
These are opening-scene throughput measurements, not racing or input-latency
benchmarks. The one-frame main-port app preset measured 56.9 fps at 720p in a
separate run with Game Mode active.

Pinyon's first 720p capture showed colored blocks over the speedometer. The
extended repeat lost parts of the minimap and gauge, despite the automated
world-capture checks passing. The 1440p captures had a clean HUD and solid stripes.
The first route's earlier menu capture also landed on a blank loading frame;
the repeat and 1440p routes therefore checked the world captures only. Logs,
commands, captures and `framerate-comparison.json` are retained locally under
`forza-recomp/out/macos-validation/tuning`.

Other isolated comparisons did not fix the material grid: disabling Metal
argument buffers alone, swapping in Pinyon's MoltenVK 1.4.1, disabling fast math,
forcing late depth/sample-mask output, and changing derivative/LOD selection.
The original MoltenVK library was restored. These negative results and captures
are retained in the ignored validation directory.

Pinyon's [exposure fix](https://github.com/arcanite24/shiftglue-sdk/commit/6e3f4343e9db500fe2c95b34998675e5913df7ea)
also addresses this port's 16 KB host-page assumption. A trial backport passed
an isolated check of neighboring, direct and crossing writes, while the old
tracker reproduced the neighboring GPU-data loss. Actual base and TU4 gameplay
then showed a corrupted speedometer/minimap. The trial was backed out, and the
0082 SDK/runtime restored. Further integration with this older Xenos renderer is
needed before adopting that memory change. The trial patch and helper remain in
`forza-recomp/out/macos-validation/diagnostic-sources`; they are not part of the
build's patch series.

A further trial of Pinyon's product-based pixel NaN handling was also rejected:
the same 25-second 1440p opening-scene window measured 27.6 fps, compared with
28.9 fps using the retained 0082 math. It did not provide a useful improvement
on this backend. The trial patch and CSV logs remain under the ignored
`macos-validation/tuning` directory.
