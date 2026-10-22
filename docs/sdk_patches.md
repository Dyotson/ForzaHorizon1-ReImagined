# ReXGlue SDK patches we carry

`patches/rexglue-sdk/` holds this project's patches to the ReXGlue SDK (nightly 0.10.0.24,
commit `bd833a2`); `tools/build_rexglue_sdk.sh` applies them to the clone
`~/rexglue-sdk-src-forza` and installs the patched GPU plugin, runtime and codegen tool into
the overlay `~/rexglue-sdk-patched-forza`. Patches 0001-0035 came from RecompiledHaloReach,
whose `docs/sdk_patches.md` describes each one (the numbers match; this repo leaves out
0010, 0012, 0014, 0024, 0026 and 0027). Patches written here (0036, 0038 and 0039 for both
projects, measured in this game's races; RecompiledHaloReach `docs/perf.md` has the profile; 0040, 0042, 0043, 0048, 0049 and 0050 are this
project's alone):

| Patch | Area | What changes | Why |
| --- | --- | --- | --- |
| 0086 | Startup pipelines (Vulkan, Direct3D 12) | Reports existing blocking warm-up progress; Vulkan persists a validated, GPU/driver-specific `VkPipelineCache` beside the shader description cache | Makes startup preparation visible and lets the driver reuse locally compiled pipeline data. Only previously encountered pipelines can be warmed; see [Startup shader warm-up](shader_warmup.md) |
| 0091 | HDR presentation and HUD coverage | Opt-in scRGB swapchain, bounded SDR highlight expansion, pre-HUD scene snapshots, conservative HUD mask, calibration and diagnostic float readback; retains the semaphore lifetime handling from 0076 | Port the PinyonShift HDR presenter without a wholesale SDK/executor update; keep HUD white controlled (#33, [setup and validation](hdr.md)) |
| 0036 | Vulkan shared memory uploads | An upload of vertex or index data the CPU rewrote no longer ends the current render pass when no draw in it has read the uploaded pages: it is recorded before the render pass, and the uploads hoisted there share one barrier pair (`vulkan_hoist_shared_memory_uploads`) | Races at 144 ran 52,000 render pass instances and 133,000 barriers a second, nearly all for these uploads (10,600 and 27,000 with the patch) |
| 0037 | Video mode (runtime) | The guest refresh rate (`video_mode_refresh_rate`, its range and `VdQueryVideoMode`'s clamp) may go up to 480 Hz instead of 240 | 144 fps (and other rates above 120) can keep the game's divider of two: a 288 Hz guest vblank. With a 144 Hz vblank and no divider, the emulated GPU's swap waits (which end on a vblank, patch 0030) turn every frame that misses 6.9 ms into a 13.9 ms one: free roam made 72 fps at a 144 fps target. `docs/framerate.md` |
| 0038 | Vulkan submission | The deferred command buffer is replayed into Vulkan and submitted on a "Vulkan Submission" thread instead of the GPU command processor thread (`vulkan_threaded_submission`) | The replay took 11% of the command processor thread, which bounds the races above 60 fps |
| 0039 | Shared memory | `clear_memory_page_state` off by default, as in Xenia (`--clear_memory_page_state=true` restores the SDK's default) | On, every page the CPU wrote and the GPU read was uploaded again after every frame: 74,000 uploads (2 GB) a second in a race at 144, 1,800 without |
| 0040 | Keyboard and mouse (runtime) | The left stick's keys ramp (`mnk_stick_ramp`, default 0.2 s: 60 % at once, full lock over the ramp, released in half of it); the mouse drives the right stick by its speed, low-passed over the game's polls (1000 px/s is half the stick at sensitivity 1, five times the SDK's old scale); `Shift`, `Control` and `Alt` work as bindings on their own | Steering with keys was full lock at once; a mouse moved at normal speeds stayed inside the game's dead zone of the look stick; a key bound to a modifier never fired. `docs/playing.md` |
| 0042 | Vulkan render targets (debug) | `vulkan_render_target_initial_fill` (default 0, unchanged behaviour): 1 clears a new host color render target to zero before its first use, 2 fills it with magenta (64, 0, 64, 1); each fill is logged | A render target the guest never wrote holds whatever the driver's memory had; filling it shows whether the game reads such memory (driver-dependent flicker, #34; `docs/rendering.md` > "Screening for driver-dependent bugs") |
| 0043 | Presenter (runtime) | `present_effect` accepts `cas` and `fsr` (AMD FSR 1: EASU upscaling, repeated up to 4x, then RCAS) on Linux and Windows, Vulkan and Direct3D 12: `REX_HAS_FIDELITYFX_SDK` is defined for the whole SDK build, no FidelityFX library needed. `fsr2` / `fsr3` stay out unless the SDK is built with the FidelityFX runtime | The nightly SDK is built without the `REXGLUE_ENABLE_FIDELITYFX` option, so `present_effect` allowed only `bilinear` and the launcher's cas / fsr / fsr2 / fsr3 entries did nothing. The CAS and FSR 1 shaders are precompiled in the SDK source (`src/ui/shaders`); only the temporal path (FSR 2/3) needs the FidelityFX library (#44; `docs/rendering.md` > "Upscaling") |
| 0048 | Resolve readback | The readback buffers of a resolve are keyed by the written range plus the copy's source (depth or color) and destination format (format, number format, endian, swap, array/slice), not the range alone. Exponent bias, EDRAM base and size stay out of the key | Photo mode (#54): the saved photo is a 1280x720 color resolve into the range a depth resolve fills every frame. With the range as key, `some` treated it as an old key and never copied it back (the CPU encoded whatever an earlier copy left there), and `fast` handed the CPU the previous resolve of that range, the depth buffer. With the copy's kind in the key it is a first-seen key: copied back after a GPU wait. A key is first-seen whenever the range alone was, so everything `some` copied before (car thumbnails) is still copied; race fps at 144 unchanged (`docs/perf.md`) |
| 0049 | Draws (Vulkan, Direct3D 12) | The memexport readback check after a draw looks at the draw's memexport ranges first, and only then at the setting | The setting check looks the legacy setting up by name (the registry lock, two `std::string`s and a hash lookup) and ran on every draw, memexport or not: in a race 8-15% of the command processor thread, most of its `malloc` and `memcpy` (#45, `docs/perf.md`) |
| 0050 | Timer queue (runtime) | On Linux and macOS the timer thread blocks on a condition variable until the next timer is due or a new one comes (disruptorplus' blocking wait strategy, whose two timed waits passed their arguments in the wrong order and did not compile); its timer slack is 1 us. Windows keeps the spin strategy (whole-tick deadlines there) | The spin strategy polled between timers (pause loops, `sched_yield` 19 times in 20, then a 1 ms sleep): with the 1 ms timestamp timer, 5-7% of a core in a race, and up to 1 ms late in its sleep phase (#45) |
| 0051 | Pipelines (Vulkan) | New setting `sync_pipeline_creation` (default off): while set, a draw whose pipeline is a placeholder waits for the creation thread's real pipeline (up to 10 s) instead of being skipped, and new pipelines are created at once, whatever `async_shader_compilation` says. Direct3D 12 not covered | A render made once and read back loses every draw skipped for a placeholder for good: with an empty pipeline cache the car's picture saved at a purchase had no car (#74). The game sets it around the picture and the livery renders of a car load (`src/hooks/thumbnail_sync.cpp`, #77); nothing else uses it |
| 0052 | Textures (Vulkan, Direct3D 12) | New setting `scaled_resolve_min_coverage_percent` (default 50; 0 = old behaviour): with a resolution scale above 1, a texture is loaded from the scaled resolve buffer only if resolves wrote at least that share of its guest memory range, not on any overlap of one page | A resolve that overlapped only the last mip pages of a 1024x1024 DXT5 texture made the whole texture a "scaled resolve" texture, read from a buffer that is zero where nothing was resolved: black (#77). Resolve targets are covered by their own resolve and stay scaled |
| 0095 | Codegen: multiply-add | `fmadd`, `fmsub`, `fnmadd`, `fnmsub` and their single forms call `ppc_fma` instead of `std::fma`. It is defined in the generated `forza_pch.h` (template `codegen/_ppc_fma.inja`): the inline FMA instruction when the code is built with FMA; otherwise `std::fma` on a CPU with FMA (results unchanged) and a multiply, then an add, on one without (CPUID FMA and OSXSAVE, XCR0 AVX state; checked once at startup). `REX_PPC_FMA=0` forces the multiply-add, `REX_PPC_FMA=1` `std::fma` | On CPUs without FMA (Ivy Bridge and older, #160, #112) `std::fma` is a software routine: about 30 ns a call in the Windows CRT (1.4 ns with FMA), 9.6 ns in glibc. A race makes 30-40 million calls a second, most on the audio thread: the sound slowed down and frames stalled. The multiply-add may differ from the fused result in the last bit. The helper is in generated code, not the SDK headers, because Linux builds use the stock nightly headers |
| 0096 | Codegen: headers as build outputs | The codegen custom command in the `generated/rexglue.cmake` template also lists the generated `<project>_pch.h` and `<project>_init.h` as outputs | The precompiled header and `main.cpp` knew those headers only from their depfiles, so ninja judged them up to date before codegen ran. When a new codegen tool changed `forza_pch.h` (0095), it was rewritten mid-build and every source compiled after it failed: "forza_pch.h has been modified since the precompiled header was built". A second build worked. Codegen rewrites `generated/rexglue.cmake` from this template on every run, so the fix belongs in the template |
| 0097 | File system: host errors | `HostPathEntry::CreateEntryInternal` creates a guest directory with the `std::error_code` form of `create_directories` and logs a refusal (`cannot create directory '<host path>': <reason> (<code>)`); `NtCreateFile` then fails with `X_STATUS_ACCESS_DENIED`, as when the directory cannot be made for any other reason. `VirtualFileSystem::OpenFile` checks that a cached entry still exists on the host the same way, and keeps the entry if the check fails | The throwing forms let `std::filesystem::filesystem_error` out of `NtCreateFile` into guest code, which ended the game (#211: Windows, saving a photo). The photo's folder is named after the file name the player types. `NtCreateFile` accepts `:` in a name, and Windows refuses it in a directory name (`ERROR_DIRECTORY`); a host path past `MAX_PATH`, or a file where the directory goes, threw too. The game handles a failed `CreateDirectoryA`. `tools/test_vfs_host_errors.cpp` |

0037 is in `librexruntime.so`, so the overlay carries a patched runtime (as for 0028).
Without it the game falls back on its own: `src/hooks/frame_time.cpp` reads the setting's
range and uses a divider of one when twice the frame rate does not fit.

0044 fixes native clang-cl SDK builds on Windows. 0045 records the Vulkan
presenter's output-pipeline format so an unchanged swapchain does not rebuild
that pipeline every paint.

0047 uses hardware implicit LOD for ordinary automatic cubemap samples on NVIDIA,
preserving mip bias and filtering. The captured traffic-car reflection shader
produced a checker pattern with manually emitted coarse gradients; implicit LOD
matched the reference result. Explicit guest gradients and LOD, and other GPU
vendors, retain their existing path. See [Rendering issues](rendering.md#nvidia-car-reflection-sampling-2026-10-15).

0064 flushes pending Vulkan shader/pipeline storage writes before sleeping and drains queued records on shutdown. See [Performance](perf.md#flush-pending-vulkan-shader-cache-writes-before-waiting-49).

0065 skips repeated Vulkan GPU-to-host resolve work after a successful CPU copy in `some` mode, preserving the first copy and cache eviction. Other modes are unchanged. See [Performance](perf.md#nvidia-vulkan-resolve-readback-overhead-49-2026-10-15).

0063 adds native ultrawide rasterization, proportional HUD layout/edge anchors and square reflection cube faces. It applies independently of DLSS; see [Ultrawide](ultrawide.md).

0053 refreshes known reused car-thumbnail readback surfaces on Vulkan; see
[Rendering issues](rendering.md#reused-car-thumbnail-readbacks-vulkan).

0067 enables the SDK's existing direct clock-conversion fallback on Linux libc++
when C++20 clock conversions are unavailable, using the standard-library feature
macro rather than an Apple-only platform check. Native libstdc++ and MSVC
conversions remain unchanged.


0055 preserves the source stencil value when copying depth/stencil ownership
without shader stencil export. Previously the stencil-only shader skipped its
bit test and filled the destination with 255, rejecting dynamic shadows (#75).
See [Rendering issues](rendering.md#missing-dynamic-shadows-in-the-stencil-copy-fallback-75).

0068 binds a window of the scaled resolve buffer, not the whole buffer, for the readback downscale
compute pass when the buffer (512 MB x scale^2) is larger than `maxStorageBufferRange`. At render
scale 3 on AMD the whole-buffer binding was invalid and the car thumbnail was saved black (#102).
Smaller buffers (scale 1 and 2 on AMD) are unchanged. See
[Rendering issues](rendering.md#black-thumbnail-at-render-scale-3-102).

0069 limits the default Vulkan frame queue to two frames on NVIDIA, retaining
three on other vendors. This shortens blocking resolve-readback waits without
skipping CPU copies or changing readback-cache eviction. The three-slot resource
ring and fence-based retirement remain in place. Set
`vulkan_max_frames_in_flight=3` to restore the previous queue depth; nonpositive
values select the vendor default, and positive values are limited to 1-3.
See [Performance](perf.md#nvidia-readback-cache-miss-stalls-49-2026-10-16).

0070 adds FIFO_LATEST_READY (`VK_EXT_present_mode_fifo_latest_ready`, or its KHR promotion) to the
Vulkan presenter: enabled with the `presentModeFifoLatestReady` feature when the device has it, and
chosen over plain FIFO when the surface offers it (after mailbox, immediate and relaxed FIFO, which
keep their cvars). New cvar `vulkan_allow_present_mode_fifo_latest_ready` (default true); false or an
unsupported driver gives plain FIFO exactly as before. The chosen mode is logged once. It evens out
the frame pacing of FIFO at high frame rates on drivers without MAILBOX (#80). See
[Frame rate](framerate.md).

0066 averages supersampled RGBA8 car-thumbnail pixels during readback, keeping other formats and resolves unchanged. Requires the known-thumbnail refresh rule already on main (0053); see [Rendering issues](rendering.md#filter-supersampled-car-thumbnails-50).

0072 adds the AMD FidelityFX FSR 3.1 upscaler (no frame generation) as a second backend of
0046's scene/UI split: `fh1_upscaler=fsr3_quality|fsr3_balanced|fsr3_performance|fsr3_native`
on any Vulkan GPU. CMake fetches the FidelityFX SDK v1.1.4 GLSL passes and headers (MIT; pinned
commit and per-file SHA-256; `REXGLUE_FSR3_SDK`, `REXGLUE_USE_FSR3`) and embeds them; the
renderer compiles them with its glslang at start-up (`CompileFh1ComputeReflected`, with an
include handler and reflection for the bindings) and `fh1_fsr3.cpp` dispatches the SDK's pass
sequence. The DLSS state (inputs, depth/motion pass, jitter, status file) is no longer NGX-only;
jitter uses FSR's phase count in FSR modes. The MIT notice is installed under
`share/licenses/amd-fidelityfx-fsr3` and copied beside the GPU library. See
[Rendering issues](rendering.md#temporal-upscaling-with-amd-fsr-3-51-2026-10-16).

0073 raises the FSR 3 and DLSS scene input to at least the game's native 1280x720 (never above the
output), logged once: FSR 3 Performance at a 1080p output (960x540) crashed a player's game (#110).
See [Rendering issues](rendering.md#temporal-upscaling-with-amd-fsr-3-51-2026-10-16).

0046 adds native Windows Vulkan DLSS/DLAA with separate scene/UI rendering.
See [Native DLSS](native_dlss.md) for runtime setup, licensing and validation limits.

0057/0058 keep DLSS viewport jitter consistent across EDRAM tiles and rotate
depth, motion vectors and NGX jitter into the final color orientation. 0062
synchronizes unscaled scene resolves into the scaled native HUD cache (1440p
Performance uses a 1x scene and 2x HUD). All three belong to the DLSS integration
and apply without the ultrawide patch series.

0071 loads eligible scaled, tiled 32-bit textures directly from the scaled resolve
buffer into an R32_UINT storage view on NVIDIA. The shader preserves the original
texel bits and endian conversion, while removing the intermediate untile buffer
and buffer-to-image copy. It handles single-layer 2D base levels only; other
formats, mip chains, arrays, 3D layouts, unsupported storage formats, and other
vendors retain the original path. Image allocation/view failures also fall back.
`vulkan_direct_scaled_texture_upload=false` restores the original uploads. The
storage descriptors use the existing frame retirement rules, and image writes
are ordered before subsequent sampling or uploads. See [Performance](perf.md#scaled-texture-upload-cost-101).

0074 adds a racing wheel input driver (`src/input/wheel/`, issue #4). An SDL joystick that is a wheel (SDL
type, a wheel-like name, or any controller without a gamepad mapping with `wheel_mode = on`) is presented as
the Xbox 360 Wireless Racing Wheel: SubType 2 with the force feedback and wireless flags (FH1 checks exactly
that when it reads the capabilities), steering on thumb LX, pedals on the triggers, buttons and hat on the pad
buttons. Known Logitech wheels (by VID:PID) get fixed axes, pedal direction and, for the G29/G923, buttons; other wheels have their axes detected from their resting values (a pedal counts only after it has been seen at rest), with overrides and curves (`wheel_*` cvars). `wheel_steer_offset` pre-compensates the game's inner steering deadzone. The
motors' vibration drives SDL haptic effects (a spring and a sine): approximate, untested on hardware. The
patch also turns on SDL's haptic subsystem in `thirdparty/CMakeLists.txt` (it was off) and the SDL gamepad
driver ignores wheels the wheel driver takes. Local builds need `libudev.h` (Homebrew `systemd`, added to
`tools/build_rexglue_sdk.sh`), otherwise SDL is built without udev and sees no joystick at all.
Test: a uinput virtual wheel; see [Playing](playing.md#racing-wheels-issue-4).

0075 adds an atomic Vulkan graphics-settings transaction. A complete request
is consumed at a closed frame, after pending submissions finish. Scale-dependent
render targets, textures, reconstruction and pipelines are replaced together;
the previous configuration is restored if initialization fails. Active shader
microcode is transferred before releasing the old cache, and shader storage is
partitioned by output and scene scale. Native scene scaling is independent of
the HUD's output size. Scene-to-HUD blits use the actual raster sizes, including
fractional 1080p output within 2x backing storage. NVIDIA device extensions are
requested when the device is created so Native sessions can enable DLSS later.
The vblank worker observes refresh-rate changes without restarting. The native UI is added separately.

0076 initializes DLSS on a private GPU submission before rendering game frames,
orders NGX scratch/history accesses, enables buffer device addresses when available,
and ties presentation semaphores to swapchain images. See
[Rendering](rendering.md#vulkan-dlss-initialization-and-presentation-lifetime).

0077 adds SMAA 1x to the FH1 scene/native-HUD bridge. It fetches pinned, hash-checked
MIT sources and lookup tables from iryoku/smaa, packages the license, and compiles
three Vulkan compute passes. SMAA bypasses NGX and temporal jitter/history. The
live graphics transaction accepts the new scene mode and suppresses final-image
FXAA while scene AA is active. Optional diagnostics log guest MSAA sample counts
without changing the guest registers or render-target layout.

0078 lets passive ImGui dialogs opt out of continuous UI repaints. The performance
overlay draws with game frames; interactive dialogs continue to request redraws
for input and animations. This prevents the FPS display itself from driving the
presenter at a higher rate than the game.
The hint lives in a private runtime side table, set through the optional C export
`rexui_SetDialogContinuousRepaint` on the UI thread and forgotten when a dialog
is removed or its drawer is destroyed. Neither ImGuiDialog nor ImGuiDrawer gains
a public member or virtual function. The game compiles with the stock nightly
headers; older runtimes without the export safely retain continuous repainting.
Linux SDK CI compiles the actual HUD adapter against those stock headers, even
on an SDK cache hit, and tests passive/mixed dialog policy and hint cleanup.

0079 registers generated functions by iterating the existing null-terminated
`PPCFuncMappings` table rather than emitting one virtual call per guest function.
AppleClang's ARM64 SelectionDAG construction spent several minutes on the base
game's 77,344-call straight-line routine, including with optimization disabled.
The table has the same filtering, addresses, targets and order as the old calls
(all 77,344 entries compared equal in the base build). The generated DLL entry
points retain their exported names and use each module's own mapping table.

0080 finalizes the SPIR-V rectangle fallback's next-program-counter `OpPhi` after
emitting its next-guest-vertex path. Previously that path appended its `(0, block)`
operands after the instruction had already been built, so shaders with guest
control flow lacked a predecessor in their Phi. On devices without geometry
shaders, Forza's tone-mapping rectangle was lost: scene textures contained the
colored world, but the screen showed only the HUD. The M4 Pro opening drive renders
after the fix and responds to throttle and steering. The geometry-shader path is
unchanged. See [macOS validation](macos.md).

0081 adds an opt-in Vulkan sampler fallback, `vulkan_force_base_mip`. It clamps
2D texture sampling to mip 0 and disables anisotropy. It remains available for
diagnostics, but is no longer enabled by the macOS play script: patch 0082 removes
the tested crosshatch without sacrificing mip filtering.

0082 allows arithmetic contraction in Apple GPU color shaders, following the
contraction portion of [Pinyon Shift's macOS pixel-math tuning](https://github.com/arcanite24/shiftglue-sdk/commit/524c6fb61cf439ac54664a4bccf5eeaf8784ba9c).
It retains this SDK's existing SM3 multiplication rules. Vertex shaders, shaders
writing depth, memory exports and synthetic depth-only shaders stay precise.
`spirv_pixel_shader_contraction` defaults on for macOS and is gated to Apple GPUs;
it requires a restart. Its value is part of the shader modification key, and the
pipeline storage version is advanced. On M4 Pro, an on/off TU4 comparison changes
crosshatched Viper stripes and roughly 21–24 fps to solid stripes and the 30 fps cap,
with normal mipmaps and anisotropy. See [macOS validation](macos.md).

A trial backport of ShiftGlue's 4 KB shared-memory tracking is **not included**.
It passed isolated neighboring/direct/cross-page invalidation checks but caused
HUD corruption in both game builds with this older Xenos renderer. The source,
regression helper and captures are retained in the ignored validation directory;
see [the Pinyon comparison](macos.md#pinyon-shift-comparison).

0083 requests Cocoa fullscreen after showing the SDL window. The request made
while the window was hidden left the tested app windowed despite `fullscreen=true`.
It preserves the requested state before creating/showing the window, since
visibility notifications can publish the initial windowed state synchronously.
It raises the visible window before entering fullscreen when fullscreen was requested.
The local app bundle now enters a native fullscreen Space on launch; macOS's
GamePolicy log confirms that Game Mode becomes active. Other platforms retain
their existing window-creation order.

0084 adds experimental Apple MetalFX temporal and spatial scene reconstruction.
It exports Vulkan images as Metal textures, imports two Metal shared events as
Vulkan timeline semaphores, and splits the Vulkan command buffer around the
native Metal pass. A separate Metal queue avoids waiting behind MoltenVK work
that depends on the upscale. There is no CPU texture readback or per-frame CPU
GPU wait. The existing game-derived depth/motion decoder and projection jitter
feed temporal reconstruction; the HUD renders afterward at 1440p. Descriptor
sets rotate across frames, and dynamic graphics state is rebound after the
command-buffer split. Ordinary submissions keep their allocation-free path.
Mac command pools reserve the continuation buffer before MetalFX is active, so
the shared graphics transaction can enable either MetalFX mode during native
play. Unsupported output sizes are rejected before replacing render caches.

The native bridge is built only on macOS. Unsupported initialization reports an
error and retains native rendering. MetalFX output is restricted to the existing
integer HUD scales; a 1080p trial cropped the UI and is rejected. A register-level
MSAA reduction trial corrupted reflections/vegetation and is not included.
See [MetalFX usage and measurements](macos.md#apple-metalfx).

0085 copies packed scene/HUD resolve texels as whole words. Patch 0100 below
corrects its original storage-layout assumption. The common 1x-to-2x
path repeats 8/16-bit texels with masks and shifts and 32/64/128-bit texels with
word loads. Other integer scales use the same nearest-sample mapping; unaligned
sources retain the original byte path. The diagnostic flag
`fh1_fast_resolve_copy=false` selects the original algorithm. This changes no
MSAA, mip filtering, shader precision, or scene/HUD resolution.

Reference comparisons covered 270 format/scale/alignment combinations. A live
TU4 MetalFX Spatial session alternated the original and optimized algorithms:
the original averaged 40.4–41.8 fps and the optimized windows 49.4–54.8 fps.
The world, HUD and solid stripes rendered correctly. These were background
opening-scene samples with concurrent media analysis, not verified 1440p/60
foreground performance. See [1440p target](macos.md#1440p-at-60-fps-target).

0090 adds Xbox impulse trigger output to the SDL input driver. The optional C
export `ForzaInputTriggerRumble(user_mask, left, right, duration_ms)` routes only
to SDL Xbox One/Series devices that advertise trigger rumble, through the same
device assignments and SDL gamepad handles as normal game input/rumble. It uses
`SDL_RumbleGamepadTriggers`, leaving body motor state intact. Calls respect the
vibration setting, UI input blockers and focus, and clamp effects to 150 ms.
Two nonvirtual methods are added; class layouts and virtual tables are unchanged.
The game discovers the export dynamically, so an older runtime still loads and
the launcher explains that an update/rebuild is required. Patch number 0090 avoids the numbers reserved by the graphics, macOS and warm-up PRs. See [telemetry](telemetry.md#xbox-impulse-triggers).

Windows validation: both modified SDK sources compiled; an isolated runtime DLL
linked and exported the entry point. A probe with real SDL virtual controllers
verified trigger amplitudes, routing to guest users, unsupported-device rejection,
body rumble preservation, UI/focus/master-vibration gating and timeout. Physical
USB, Bluetooth and Xbox Wireless output and Linux builds remain untested.

### Photo directory failures (#211)

0097 keeps host directory-creation errors inside the existing guest filesystem
failure path. `HostPathEntry::CreateEntryInternal` previously used the throwing
`std::filesystem::create_directories` overload. A host filename rejected by Windows,
a file occupying the directory name, or access denial could therefore unwind out
of `NtCreateFile` and abort the process. The error-code overload logs the path and
host error, then returns null; `VirtualFileSystem::OpenFile` already translates
that failure to `X_STATUS_ACCESS_DENIED`. No exception catch or success fallback
hides a failed save. The patch also makes cached host-existence checks non-throwing.

Reproduced on Windows/D3D12 with a copied profile and an existing private TU4
development host: take a normal-size photo and save it as `invalid:Photo1`.
A private naming-dialog fixture supplied the text through the runtime's usual
asynchronous completion; the baseline filesystem code was unchanged. The process
aborted with `lr=831AD110` and all eleven guest return addresses matching the
reporter's log. The reporter's actual filename and host error are unknown, so
the colon is a demonstrated trigger rather than a claim about their input.

With the candidate, that same native D3D12 save reports **FILE OPEN FAILED** and
returns to the pause menu after dismissal, instead of aborting. A normally named
photo on Vulkan reports **SAVE SUCCESSFUL** and writes its image, thumbnail and
metadata to the copied profile. The source fixture's hashes are unchanged. These
are development-host functional checks, not a full rebuild of current main, a
photo-quality assessment, a large-photo upload test, or a reporter retest.

`tools/tests/sdk-filesystem` builds the real-runtime test from
`tools/test_vfs_host_errors.cpp` on Windows, in addition to #213's Linux coverage.
The Windows SDK job runs it before packaging and caching the overlay. Full CI
builds and reporter retesting remain necessary.

### Cross-scale scene/HUD resolve layout

0100 corrects the scene/HUD resolve mirror to resample the SDK's repeated
16-byte storage quads with the resolve's byte order. It supersedes the
individual-pixel mapping in 0046/0085, retaining whole-word loads where safe and
the `fh1_fast_resolve_copy` diagnostic switch. Both paths pass the standalone
Vulkan reference matrix, including transfers to/from 3x. See
[cross-scale minimap corruption](rendering.md#cross-scale-minimap-corruption-at-3x).

0102 adds an opt-in native Vulkan depth/motion/blur pass to the draw stream.
`FORZA_NATIVE_RENDERER=velocity` replaces the recognized full-screen motion draw;
`validate` executes the native pass alongside the original. Unsupported states,
active occlusion queries and fractional scene rasters retain the original draw.
The deferred command carries an immutable descriptor/image snapshot, with slots
reused only after their submission fences complete. Shutdown drains recording and
GPU work before releasing the pass. The HUD continues through the existing path.

The SDK's `REXGLUE_FH1_NATIVE_SOURCE_DIR` points at the canonical implementation in
`forza-recomp/src/platform/native_renderer`; project build scripts and release CI
set it explicitly. An empty path builds a standalone SDK with a fallback bridge,
and a nonempty but incomplete path fails configuration. The shader header is
generated in the build directory. Native rendering is disabled by default and
does not require NVIDIA libraries. See [Native renderer](native_renderer.md) for
the deliberately limited replacement scope and validation evidence.

0103 adds `FORZA_NATIVE_RENDERER=velocity-direct`. The native motion pass writes
its RGBA8 output directly into the existing color attachment, removing the
intermediate blur image and copy. The render-target owner handles the borrowed
image's layout and visibility; the pass retains its own depth/motion images and
submission-protected descriptor slots. Only compatible single-sample RGBA8
attachments receive storage usage. Unsupported formats and states retain the
original draw. This does not yet bypass EDRAM resolves or replace geometry.

0104 adds `FORZA_NATIVE_RENDERER=velocity-resolve`. For an immediately preceding
native motion output, a native RGBA8 image-to-buffer shader replaces the EDRAM
dump and resolve with one dispatch. It preserves the existing tiled/scaled
texture buffer, upload, readback and clear contracts. Integer-scale,
single-sample unsigned RGBA8 layouts are supported; conversions, array targets,
half-pixel fill, MSAA and other sources retain the original resolve. The source
owner tracks image visibility, while existing buffer range and descriptor
retirement rules order consumers. Canonical shader/layout sources and their BSD
mapping license are supplied through `REXGLUE_FH1_NATIVE_SOURCE_DIR`. The feature
is off by default. See [Native renderer](native_renderer.md#native-rgba8-resolve)
for GPU equivalence and actual-game validation.

0105 adds `FORZA_NATIVE_RENDERER=rgba8-resolve`, selecting a fully owned source
from the SDK render-target ownership map. Only a complete, non-wrapping region
owned by one matching single-sample RGBA8 target uses the native image-to-buffer
resolve. Mixed ownership, reinterpreted sources and unsupported conversion states
retain the original path. The original motion draw remains active in this mode
for independent resolve validation. It is disabled by default and does not
replace geometry. See [Native renderer](native_renderer.md#resolve-from-tracked-attachment-ownership).

0106 carries the SDK's half-pixel edge-fill flag into the native RGBA8 resolve.
The top/left gap samples the first surely covered texel at half the integer
scale, independently on each axis. The native destination mapping and ownership
checks are unchanged. Runtime endian modes are limited to 0..3 to match the
existing fast 32-bit SDK shader; 64/128-bit swaps retain its original path. This
allows the captured full-scene RGBA8 case to bypass EDRAM while preserving the original scaled resolve output. See
[Native renderer](native_renderer.md#full-scene-half-pixel-edge-filling) for
original-GPU byte comparisons and synthetic coverage.

0107 adds the independent `FORZA_NATIVE_RENDERER=geometry` development mode.
It replaces only a verified rigid depth/stencil draw using an authored native
vertex shader and immutable GPU-source vertex/index snapshots. The bridge gates
shader identity, constant maps, primitive, depth/stencil, raster, target format
and sample count; unsupported draws retain the original path. Copies precede
the draw outside a render pass, source read tracking orders later writes, and
all affected graphics state is restored on the next original draw. Slots retire
with waited submissions and are released after recording/GPU shutdown. The
canonical native pipeline supports reversed viewport depth ranges and both
traditional render passes and dynamic rendering. Standalone
SDK builds keep a fallback stub. See
[Native renderer](native_renderer.md#live-rigid-depth-integration).

0108 adds `FORZA_NATIVE_RENDERER=combined`, enabling the validated rigid-depth,
direct-motion and ownership-based RGBA8-resolve paths in the same game session.
It preserves the independent diagnostic modes and every existing contract gate;
unsupported draws and resolves continue through the original path. See
[Native renderer](native_renderer.md#combined-native-execution).

0109 adds the separately selected `FORZA_NATIVE_RENDERER=world-color` pass:
authored world-color filtering, bloom, exposure, grading and output composition
with immutable GPU quad snapshots and per-region controls. Unsupported states
retain the existing draw. See [Native world-color composition](native_world_color.md).

0110 adds `FORZA_NATIVE_RENDERER=world-color-transfer`. A guarded R10 resolve
reads the tracked scene attachment directly, applying the original exposure
conversion while writing canonical tiled backing and an immutable packed image.
A matching texture load copies that image instead of untiling the backing.
Shared-memory invalidation, waited reader/producer fences and bounded allocations
protect reuse; unsupported states retain the original path. See
[Native scene transfer](native_scene_transfer.md) for exact-output and GPU tests.

0111 lets a subsequent supported R10 producer populate its previously observed
consumer's cache-owned image directly through its R32_UINT storage view. This
removes repeated snapshot copies for the exact key while preserving canonical
backing, normal texture views, usage barriers, eviction and memory watches.
Initial consumers and missing storage views retain 0110's snapshot fallback.
