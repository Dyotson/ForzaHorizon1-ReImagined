# Native NVIDIA reconstruction (Windows development branch)

Settings > Display > NVIDIA DLSS offers Off, Performance (720p to 1440p), and
DLAA (native 1440p). These modes use NVIDIA's NGX Vulkan implementation on a
supported RTX GPU and driver. They are separate from the presenter's spatial
upscaling filter.

On first Play, the launcher downloads the runtime and its license directly from
NVIDIA's public DLSS SDK, verifies pinned SHA-256 hashes, and stores them under
`<data folder>/graphics/nvidia-dlss/<SDK revision>`. Later launches reuse verified
files offline; changed or incomplete files are downloaded again. The download
runs in the background and can be cancelled. No manual DLL copying or system
installation is needed. Completed downloads are moved atomically into the private
runtime directory, rather than retained as a second DLL copy. Verification runs
on the background preparation task; unchanged files reuse a hash cached by path,
mtime, size and file identity. Settings previews do not verify binaries. The
launcher removes its own status file when the game session ends.

Both modes select Vulkan, a 1440p output, and render scale 2 for the UI. Performance
uses separate 1x scene render-target, texture and graphics-pipeline caches, then reconstructs the
1280x720 scene into the native 2560x1440 target before HUD draws. DLAA keeps the
scene at 2560x1440 and uses NGX's DLAA mode. The existing 2x texture cache is shared
in DLAA so scaled resolves remain coherent. Small title-screen intermediate
targets do not trigger the scene/UI split; a depth-only HUD draw defers the transfer
until its color attachment exists. Each pipeline cache keeps its matching render-
target cache and shader translator, including scale-dependent VPOS and memexport
code. Shader storage for the scene is separate from the native-HUD pipeline store.

Depth and camera/object reprojection come from the game's resolved depth/stencil
texture and the constants of its `PPMotionBlurDepthHorizonVel` pass. A compute pass
reconstructs pixel motion without that shader's motion-blur scaling or clamping.
The viewport receives a subpixel jitter on main-scene depth draws; the HUD and
post-processing viewports do not. NGX executes on the Vulkan submission thread,
with its frame inputs copied into the deferred command stream. Guest pipeline and
descriptor bindings are invalidated afterward. Input is the tonemapped scene,
so the NGX HDR-input flag is not set. Jitter begins only after a successful NGX
evaluation; failure publishes a disabled state to the draw thread and subsequent
frames use zero jitter. Already-recorded in-flight frames cannot be retroactively
changed. NGX logging is off by default. Diagnostic environment and Vulkan dispatch
lookups are cached rather than repeated for every draw or blit.

The launcher reports active reconstruction only after the renderer successfully
evaluates a frame. If NGX or an auxiliary cache cannot initialize, the game keeps
native rendering and reports the failure. An incompatible render scale or render-
target path (including an extra-argument override) also disables reconstruction
instead of aborting GPU initialization. If feature creation or evaluation fails later, it reports
the temporary bilinear fallback and asks the player to disable DLSS and restart
for native rendering. Transfer usage on color render targets is enabled only for
the active scene/UI split. NGX-only Vulkan extensions and float16 are requested
only for the NVIDIA reconstruction path; DLSS Off keeps the ordinary feature set.
Details remain in the game log.

SDK patch 0046 contains the renderer integration. Windows SDK builds fetch pinned
NGX headers, the static loader and its license automatically. An existing local
SDK may be selected with `REXGLUE_DLSS_SDK`; `REXGLUE_USE_DLSS=OFF` builds without
NGX. The NVIDIA SDK license is copied beside the GPU DLL and into the SDK's license
directory. NVIDIA binaries are not committed to this repository. The pinned SDK
revision is `374959484e79a640feaba44c93ac8cfb0a03f5b5`; its
[license](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/LICENSE.txt)
applies to those components. Both launcher staging and `tools/build_windows.sh`
include the renderer capability metadata and NVIDIA SDK notice in packaged builds.

This remains a development integration limited to 16:9 1440p output. Gameplay,
title-screen startup and native HUD composition have been exercised locally;
camera cuts, transparency and all race/menu transitions need broader testing.
The separate NVIDIA traffic-car blowout was reproduced with DLSS disabled and
traced to automatic cubemap sampling. SDK patch 0047 uses hardware implicit LOD for
those fetches on NVIDIA, retaining mip bias and filtering; register gradients and
explicit guest LOD remain explicit. The old path reproduced the defect in the live
comparison, while the correction showed clean traffic in all 16 captured frames.
See [Rendering issues](rendering.md#nvidia-car-reflection-sampling-2026-10-15).

A subsequent moving gameplay recording still showed a colored artifact at the
edge. The shader-cache correction is not yet established as a fix for every
edge artifact. The stationary traffic comparison does not establish that all motion
artifacts are fixed; that case remains under investigation.

The independent Windows build, Vulkan output-pipeline and NVIDIA cubemap fixes
were merged separately in [PR #58](https://github.com/Dyotson/ForzaHorizon1-ReImagined/pull/58).
This DLSS draft is based on main after that merge.

Review validation (RTX 5060 Ti, Windows): the rebuilt SDK ran DLSS, DLAA and Off;
forced NGX creation/evaluation failures continued in bilinear fallback. An
injected evaluation failure was followed by `jitter disabled; offsets=0,0`.
An extra-argument scale-1 override and `render_target_path_vulkan=fsi` continued
with native rendering rather than failing GPU setup. A 75-second isolated TU4
session through the launcher's plan/session path reached active DLSS with native
1440p HUD and game reprojection, including driving, and cleaned up its status file.
The 16 runtime/packaging tests and offscreen success/cancel/failure worker checks
passed. These checks do not establish that the remaining edge artifact is fixed.
