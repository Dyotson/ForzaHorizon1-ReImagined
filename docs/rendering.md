# Rendering issues

## Fixed: hard-edged rectangle in the speed blur (2026-10-14)

**Symptom.** At speed the game blurs the edges of the screen. A rectangle around the
player's car was never blurred, and its edge was sharp; on the console the blur fades in
smoothly toward the car.

**Cause.** A shader constant set for the console's texture fetch, compared against the
host's exact value. Found with RenderDoc captures in the Millennia Masters race (TU4 build,
60 fps, full throttle, about 14 s after GO), analysed with qrenderdoc Python scripts (draw
list with shader hashes, targets and viewports; texture saves; the stencil state of every
draw; raw constant buffers).

- The scene is drawn 4x MSAA in three EDRAM tiles; the post-processing runs once at the end
  of the frame. Before the scene, in each tile, one rectangle-list draw marks a ground
  rectangle around the player's car with stencil 128 (depth test and write on, stencil
  always/replace, alpha test off). The scene's draws then replace the stencil with their
  object's value where they pass (0 for the world, 15 for the player's car, 3-24 for the
  other cars), so the rectangle keeps 128 on the ground around the car.
- Velocity pass (`PPMotionBlurDepthHorizonVel`, ucode `E68FE55ECD482EC5`): one full-screen
  rectangle into a 640x360 target. It reads the resolved depth and stencil as an 8888 texture
  (stencil in the low byte, float24 depth above it) and picks the reprojection matrix by
  the stencil: `stencil >= c253.w` means world (matrix c32-c35); otherwise object
  `stencil / 3`, matrix `c[32 + 4 * id]` (the player's car, 15, gets c52-c55, which
  follows the camera: zero velocity).
- `c253.w` is a literal of that pixel shader: 0x3F008100, 128.0019 / 255. The host fetch
  returns exactly 128 / 255 (0x3F008081) for a stencil of 128, which is below it. So 128
  counted as object 43, whose constants (c204-c207) happened to hold an identity matrix:
  the whole rectangle got zero velocity and stayed sharp, and the combine
  (`BloomTonemap_MotionBlur_Vignette`, five scene taps along the velocity) blurred
  everything around it. 0x3F008100 is (0x8080 + 1) / 65536: the value an 8-bit 128 gives
  if the fetch widens it to 16 bits and rounds up. That suggests the console's fetch
  returns a few ulps more than 128 / 255 and the rectangle counts as world there. An
  object index of 43 means nothing in this table either way.
- The other passes were checked and ruled out: no scissor, viewport, mask texture or
  rectangle on the blur draws; the velocity is filtered linearly; the 2-tap pass after the
  velocity (`ECE830AC0333767F`) only copies the pixel below at depth edges.

**Fix.** `src/platform/motion_blur_fix.cpp` finds the shader's literal block (c252-c255)
in the executable's image before the game starts and sets the threshold to 0.5 (between
127 and 128). It is found the same way in the disc's executable and Title Update 4's
(0x821C30F0 in TU4). `FORZA_BLUR_THRESHOLD=0` leaves the image alone, for comparisons.
The blur now fades toward the car, and the fence and the road beside it blur as on the
console. GPU work is unchanged (one constant), so the frame rate is unchanged.

**Not done.** Xenia was not compared: on Linux it only takes pad input, and a global
virtual pad is not allowed. Other shaders that compare 8-bit values against exact
thresholds may show the same off-by-ulps difference; none is known.

**What the code shows (sub_ addresses of the disc executable).**
- `sub_82461108` draws the technique `PPMotionBlurDepthHorizonVel` (a velocity pass from
  depth): a full-screen quad given as normalized shorts (-32767..32767).
- `sub_82402678` uses `BloomTonemap_MotionBlur_Vignette`, probably the final combine that
  applies the blur.
- The camera definitions set a `MotionBlurAmount` per shot (`media/camera.zip`, for
  example `321Cams.xml`).


## Screening for driver-dependent bugs (#34, 2026-10-15)

Issue #34 (Windows 11, RTX 3080) reported flicker and greenish reflections on Direct3D 12
(#46), flicker and coloured artifacts at render scale 3 (#47), and traffic cars that blow
out into a cyan glow (#48), "mainly on NVIDIA". The test machine has a Radeon (RADV), so
these checks look for behaviour that depends on the driver: if RADV, lavapipe and
vkd3d-proton all agree and the validation layers are quiet, the bug is in what one driver
does with an undefined case.

**What was run (free roam, Xenia save copy, 2-3 min of driving, screenshots every 2 s):**

| Check | Result |
| --- | --- |
| Vulkan, RADV, scale 1, 3, 4 | clean |
| Vulkan, lavapipe (CPU), scale 1 | same picture as RADV |
| Direct3D 12 (Windows build, vkd3d-proton on RADV), scale 1 and 3 | clean, no green tint |
| Validation layers + synchronization validation, RADV | one hazard, harmless (below) |
| Every new color render target filled with magenta first (scale 3) | no magenta anywhere: the game does not read render target memory it never wrote |
| NaN / Inf / values above 31.875 in the float targets, after every 20th draw of a captured frame | none; the 7e3 targets peak at exactly 31.875 |

The one validation report is `SYNC-HAZARD-WRITE-AFTER-WRITE` between two `vkCmdCopyBuffer`
into the same readback buffer in consecutive submissions with no barrier
(`IssueCopy_ReadbackResolvePath` with `readback_resolve = some`: the copy is recorded for every
resolve, the CPU reads only on a cache miss, after waiting for the GPU). Formally a race, in
practice the same bytes are written twice; not a cause of the reports. The other report,
"fragment shader writes Location 0 with no attachment", is a performance warning.

**A likely mechanism, not confirmed.** On the host render target path (the default on every
vendor but Intel, Vulkan and D3D12), pixel shader colours go to the float16 targets that stand
in for the console's 7e3 format **unclamped**. The console clamps to [0, 31.875] and turns NaN
into 0 before blending; the SDK's interlock path does the same, the host path does not. A
shader that gives Inf, NaN or a huge value on one driver (denormals, `0 * inf`, `log(0)`)
then feeds the bloom on that driver only. Clamping there would match the console on every
driver; it needs a capture from an affected machine first to see which draw.

### How to run the checks

- **Validation layers** (user install, no root): `dnf download vulkan-validation-layers`,
  unpack with `rpm2cpio ... | cpio -idm` into `~/.local/opt/vvl`, and copy
  `usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json` into
  `~/.local/opt/vvl/layers/` with `library_path` set to the absolute path of
  `usr/lib64/libVkLayer_khronos_validation.so`. Then run the game (`tools/live_session.sh`
  passes the environment through) with
  `VK_ADD_LAYER_PATH=~/.local/opt/vvl/layers VK_LOADER_LAYERS_ENABLE='*khronos_validation'
  VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn
  VK_KHRONOS_VALIDATION_DEBUG_ACTION=VK_DBG_LAYER_ACTION_LOG_MSG
  VK_KHRONOS_VALIDATION_LOG_FILENAME=/path/vvl.txt
  VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT=3` and `--vulkan_log_debug_messages=false`
  (keeps forza.log small). Free roam stays playable with synchronization validation on.
  GPU-assisted validation (`VK_KHRONOS_VALIDATION_GPUAV_ENABLE=true`) is much slower.
- **lavapipe:** `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`. The game
  runs at about 10 guest fps and 2-3 presented fps, so scripted inputs need three times the
  waits (the title screen takes 60 s, loading into free roam about 4 min), and a screenshot
  trigger should be touched without waiting for the file. A RenderDoc capture made on RADV
  does not replay on lavapipe ("tried to allocate memory from index 3, but on replay we only
  have 1 memory types"), so compare screenshots of the same place instead.
- **Direct3D 12 without Windows:** `tools/build_windows.sh`, then from the package folder
  `WINEPREFIX=<scratch prefix> GAMEID=0 PROTONPATH=GE-Proton11-1 umu-run ./forza.exe
  --gpu_backend=d3d12 --game_data_root=Z:\...` (Windows paths under `Z:`). The input FIFO is
  POSIX-only: use the `FORZA_AUTOPRESS="8:START,10:START,...,95:RT:40"` schedule. Set
  `FORZA_FRAMEDUMP_DIR` (a `Z:` path) or screenshots land in `/tmp`.
- **Uninitialized render targets:** `--vulkan_render_target_initial_fill=2` (SDK patch 0042)
  fills every new color render target with magenta (64, 0, 64, 1) before its first use; any
  magenta on screen is render target memory the game reads without writing, which is zero on
  some drivers and old data on others. `=1` fills with zero. Each fill is logged
  ("filled new render target", first 64).
- **NaN / Inf / out of range per draw:** capture a frame (`renderdoccmd capture -w -c DIR/cap
  ./forza ...` with `VK_ADD_IMPLICIT_LAYER_PATH=~/renderdoc/layer`, so the RenderDoc layer is
  found, and `FORZA_RDCAPTURE_TRIGGER`), then
  `RDC_CAP=cap.rdc RDC_OUT=DIR qrenderdoc --python tools/rdc_float_scan.py`: the first draw that
  leaves NaN, Inf or more than 31.875 in each float target, with texel positions. For a blown-out
  object, take RenderDoc's pixel history on it from there.
- **A colour on screen:** a short script over the screenshots (Pillow) that counts saturated
  pixels of the reported colour (cyan: G and B above 215, R 70 below G) finds the frames to look at;
  check the hits, the radio HUD's logo is cyan too.

Xenia Canary was not compared: on Linux it takes pad input only, and no global virtual pad is
allowed on the test machine.


## Upscaling (#44, 2026-10-15)

**What the launcher offered did nothing.** `present_effect` listed bilinear, cas, fsr, fsr2 and
fsr3, but the nightly SDK (and so our runtime) is built without the `REXGLUE_ENABLE_FIDELITYFX`
option: the cvar allowed only `bilinear` and the other values were rejected with a log warning.
SDK patch 0043 builds the spatial effects in without the FidelityFX library (their shaders are
precompiled in the SDK source), on Vulkan and Direct3D 12: `present_effect` = `bilinear`, `cas`
(sharpen, up to 2x) or `fsr` (AMD FSR 1: EASU upscaling, repeated up to 4 times for scales over
2x, then RCAS). Not used: `present_cas_additional_sharpness`, `present_fsr_sharpness_reduction`
(default 0.2 stops) stay at their defaults.

**fsr2 / fsr3 are not available, and would not work as written.** They need the FidelityFX
runtime (Linux also needs its shaders precompiled by hand), which the SDK does not ship. The
SDK's temporal path (`vulkan_presenter.cpp`, `DispatchTemporalUpscaler`) passes the colour
image as the depth buffer and as the motion vectors, a jitter of 0, a fixed camera
(near 0.1, far 1000, fov 60) and `reset = true` on every frame: no history is ever used, so
it is a spatial upscaler with wrong inputs. The launcher no longer lists them (a stored value
becomes `fsr`); the cvar rejects them without the runtime.

**Measurements** (Linux, Vulkan, RADV, RX 9060 XT; free roam at the spawn, car standing,
`frame_rate = 60`, 4 s windows of the last 30 s; headless gamescope gives the output size;
screenshots of the final output). CONTENDED: two other game instances ran the whole time
(GPU 50-60 % busy without ours), so absolute numbers are low-confidence; the ordering is clear.

| Output | Render scale | Filter | fps | p95 ms | Sharpness (banner edge energy) |
| --- | --- | --- | --- | --- | --- |
| 1080p | 1 | CAS | 59.9 | 19.1 | 29.8 |
| 1080p | 1 | FSR 1 | 59.9 | 18.4 | 38.6 |
| 1080p | 2 | bilinear | 41.9 | 28.9 | 35.4 |
| 1440p | 1 | bilinear | 60.0 | 18.2 | 15.9 |
| 1440p | 1 | FSR 1 | 59.9 | 18.4 | 28.5 |
| 1440p | 2 | bilinear | 42.5 | 29.2 | 35.9 |
| 1440p | 3 | bilinear | 26.0 | 47.2 (p99 124) | 35.4 |
| 4K | 1 | bilinear | 59.9 | 17.9 | 8.7 |
| 4K | 1 | FSR 1 | 60.0 | 18.3 | 16.8 |
| 4K | 2 | bilinear | 41.9 | 29.2 | 16.5 |
| 4K | 2 | FSR 1 | 41.9 | 29.6 | n/a |
| 4K | 3 | bilinear | 28.0 | 42.6 (p99 104) | n/a |

- FSR 1 at scale 1 costs about nothing in fps (GPU busy +0-11 points at 4K, contended) and
  recovers about two thirds of the edge sharpness that a native 2x render has; text and
  outlines get crisp. It cannot invent detail: the 2x render shows texture detail (grilles,
  small print) that FSR 1 does not, and flat textures can look painted.
- Scale 2 is real detail for about 30 % fewer fps here; scale 3 halves the frame rate and
  breaks the 60 fps target (p99 above 100 ms). FSR on top of scale 2 changes little.
- The scene was not identical between runs (the spawn view and the traffic vary), so the
  sharpness numbers come from the screen-space "Millennia Masters" banner, which is the same
  in every run; RMS differences between runs are not comparable.

**Defaults and advice.** First-run launcher default: `present_effect = fsr` at render scale 1
(a stored choice is kept). 1080p and above: FSR 1 at scale 1; a fast GPU can take scale 2 for
detail (expect about 40 fps at a 60 fps target; 30 fps is fine); stay at or below 2. Not
tried: guest video modes below 720p for slow GPUs (Steam Deck, #45).

**Temporal upscaling (FSR 2/3, DLSS): feasible, large.** Not started.
- Depth: the main scene's depth is resolved to a texture for the velocity pass; the Vulkan
  render target cache could export that image to the presenter (new plugin-to-presenter
  interface; the scene pass is found by its shader or target).
- Motion vectors: the game's velocity pass `PPMotionBlurDepthHorizonVel` writes camera and
  per-object (stencil id) reprojection velocity into a 640x360 target, only while motion blur
  is on and scaled by `MotionBlurAmount`; it needs decoding to FSR's convention and an upsample.
  Wheels and the player's car (zero velocity, camera-relative) are fine; transparencies are not.
- Jitter: the emulator owns the viewport transform (`ndc_offset` in `graphics/util/draw.cpp`),
  so a sub-pixel jitter can be added to the scene draws without the game, if the scene targets
  are identified so the post-processing and HUD are not jittered.
- The catch: a presenter-side upscaler sees the final image (tonemapped, motion-blurred, HUD
  on top); ghosting of HUD elements needs a reactive mask. Doing it before the post passes
  means one render scale for scene and post targets, which the SDK does not separate.
  Also needs the FidelityFX runtime built (shaders on Linux) and 60 fps or more to look good.
- DLSS needs NVIDIA's SDK and an NVIDIA GPU; not tested here (AMD only).


## NVIDIA car reflection sampling (2026-10-15)

**Reproduced.** Traffic cars on the Windows RTX 5060 Ti showed a colored checker
pattern that bloom turned into a white/cyan/magenta glow. It occurs with DLSS off
as well. Clamping the 7e3 output range before blending did not fix it and that
experiment was reverted. Disabling anisotropic filtering also reproduced the
blowout; the road's filtering need not be reduced.

**Capture evidence.** A native 2560x1440, 120-FPS-cap run under RenderDoc triggered
its capture when the glow was visible. The material buffer already contains the
checker pattern before bloom. Pixel history identified the Durango body draw;
the captured GPU pixel was approximately `(0.4363, 2.9199, 0.2661)`. Replacing the
shader with an identical binary reproduced that pixel. The shader validates with
the device's enabled scalar-block layout. All six reflection cubemap faces and
mip levels contained valid-looking data, rather than an uninitialized mip chain.

The CPU shader debugger (with only its unsupported explicit FP32 RTE declaration
removed in the diagnostic copy) produced `(0.22796, 0.06113, 0.09175)`. Removing
that declaration in the GPU shader did not fix the corruption. Forcing the cube
sample to mip zero removed the body blowout, but would sacrifice normal mip
selection. Replacing its explicit coarse-gradient cube samples with implicit LOD
instead gave `(0.22791, 0.06113, 0.09174)` on the GPU, matching the reference after
float16 quantization. The cause is narrowed to this sampling path; the precise
NVIDIA driver/compiler failure is not established.

**Correction.** SDK patch 0047 uses implicit LOD for ordinary automatic cubemap
fetches on NVIDIA. Fetch-constant and instruction mip bias still apply, and the
sampler retains its filters and anisotropy. Guest register gradients and explicit
LOD keep their explicit path. Other GPU vendors are unchanged. The startup cvar
`vulkan_implicit_cube_lod=false` restores the previous path for comparisons.

**Live validation.** Two 105-second native-1440p runs used the same renderer binary,
profile copy, 16x anisotropic filtering and 120 FPS cap, changing only that cvar.
The old path reproduced the blowout. The correction showed normal materials in all
16 captured traffic frames, including close passes. A fixed road-region detector
counted up to 1,850 almost-white pixels with the old path and zero in that region
across the corrected captures; this is a scene-specific check, not an all-game
artifact detector. Screenshots and the RenderDoc capture are retained locally.

This correction is independent of DLSS and does not establish that every moving-scene
artifact is resolved.
