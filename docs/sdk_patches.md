# ReXGlue SDK patches we carry

`patches/rexglue-sdk/` holds this project's patches to the ReXGlue SDK (nightly 0.10.0.24,
commit `bd833a2`); `tools/build_rexglue_sdk.sh` applies them to the clone
`~/rexglue-sdk-src-forza` and installs the patched GPU plugin, runtime and codegen tool into
the overlay `~/rexglue-sdk-patched-forza`. Patches 0001-0035 came from RecompiledHaloReach,
whose `docs/sdk_patches.md` describes each one (the numbers match; this repo leaves out
0010, 0012, 0014, 0024, 0026 and 0027). Patches written here (0036, 0038 and 0039 for both
projects, measured in this game's races; RecompiledHaloReach `docs/perf.md` has the profile; 0040, 0042, 0043 and 0048 are this
project's alone):

| Patch | Area | What changes | Why |
| --- | --- | --- | --- |
| 0036 | Vulkan shared memory uploads | An upload of vertex or index data the CPU rewrote no longer ends the current render pass when no draw in it has read the uploaded pages: it is recorded before the render pass, and the uploads hoisted there share one barrier pair (`vulkan_hoist_shared_memory_uploads`) | Races at 144 ran 52,000 render pass instances and 133,000 barriers a second, nearly all for these uploads (10,600 and 27,000 with the patch) |
| 0037 | Video mode (runtime) | The guest refresh rate (`video_mode_refresh_rate`, its range and `VdQueryVideoMode`'s clamp) may go up to 480 Hz instead of 240 | 144 fps (and other rates above 120) can keep the game's divider of two: a 288 Hz guest vblank. With a 144 Hz vblank and no divider, the emulated GPU's swap waits (which end on a vblank, patch 0030) turn every frame that misses 6.9 ms into a 13.9 ms one: free roam made 72 fps at a 144 fps target. `docs/framerate.md` |
| 0038 | Vulkan submission | The deferred command buffer is replayed into Vulkan and submitted on a "Vulkan Submission" thread instead of the GPU command processor thread (`vulkan_threaded_submission`) | The replay took 11% of the command processor thread, which bounds the races above 60 fps |
| 0039 | Shared memory | `clear_memory_page_state` off by default, as in Xenia (`--clear_memory_page_state=true` restores the SDK's default) | On, every page the CPU wrote and the GPU read was uploaded again after every frame: 74,000 uploads (2 GB) a second in a race at 144, 1,800 without |
| 0040 | Keyboard and mouse (runtime) | The left stick's keys ramp (`mnk_stick_ramp`, default 0.2 s: 60 % at once, full lock over the ramp, released in half of it); the mouse drives the right stick by its speed, low-passed over the game's polls (1000 px/s is half the stick at sensitivity 1, five times the SDK's old scale); `Shift`, `Control` and `Alt` work as bindings on their own | Steering with keys was full lock at once; a mouse moved at normal speeds stayed inside the game's dead zone of the look stick; a key bound to a modifier never fired. `docs/playing.md` |
| 0042 | Vulkan render targets (debug) | `vulkan_render_target_initial_fill` (default 0, unchanged behaviour): 1 clears a new host color render target to zero before its first use, 2 fills it with magenta (64, 0, 64, 1); each fill is logged | A render target the guest never wrote holds whatever the driver's memory had; filling it shows whether the game reads such memory (driver-dependent flicker, #34; `docs/rendering.md` > "Screening for driver-dependent bugs") |
| 0043 | Presenter (runtime) | `present_effect` accepts `cas` and `fsr` (AMD FSR 1: EASU upscaling, repeated up to 4x, then RCAS) on Linux and Windows, Vulkan and Direct3D 12: `REX_HAS_FIDELITYFX_SDK` is defined for the whole SDK build, no FidelityFX library needed. `fsr2` / `fsr3` stay out unless the SDK is built with the FidelityFX runtime | The nightly SDK is built without the `REXGLUE_ENABLE_FIDELITYFX` option, so `present_effect` allowed only `bilinear` and the launcher's cas / fsr / fsr2 / fsr3 entries did nothing. The CAS and FSR 1 shaders are precompiled in the SDK source (`src/ui/shaders`); only the temporal path (FSR 2/3) needs the FidelityFX library (#44; `docs/rendering.md` > "Upscaling") |
| 0048 | Resolve readback | The readback buffers of a resolve are keyed by the written range plus the copy's source (depth or color) and destination format (format, number format, endian, swap, array/slice), not the range alone. Exponent bias, EDRAM base and size stay out of the key | Photo mode (#54): the saved photo is a 1280x720 color resolve into the range a depth resolve fills every frame. With the range as key, `some` treated it as an old key and never copied it back (the CPU encoded whatever an earlier copy left there), and `fast` handed the CPU the previous resolve of that range, the depth buffer. With the copy's kind in the key it is a first-seen key: copied back after a GPU wait. A key is first-seen whenever the range alone was, so everything `some` copied before (car thumbnails) is still copied; race fps at 144 unchanged (`docs/perf.md`) |

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
