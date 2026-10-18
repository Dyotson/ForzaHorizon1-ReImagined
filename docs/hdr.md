# HDR output (#33)

HDR output is opt-in under **Settings > Display > HDR**. It requires Vulkan,
an HDR-capable display, HDR enabled in the desktop settings, and a Vulkan surface
offering `R16G16B16A16_SFLOAT` with `EXTENDED_SRGB_LINEAR_EXT`. Otherwise the
presenter logs the reason and keeps ordinary SDR. Direct3D 12 does not use this
implementation; the launcher disables its HDR controls for that backend.

This expands highlights from the game's already tone-mapped SDR picture. It
cannot recover clipped source detail or provide game-authored HDR lighting.
The output is linear scRGB (1.0 = 80 nits), composed by the operating system.
Do not combine it with another automatic HDR conversion.

## Setup

1. Enable HDR for the display in Windows Settings (or a supported Linux HDR
   desktop). Start with the ordinary Vulkan rendering path.
2. Turn on **HDR output** in the launcher. Set **Display peak** to the display's
   actual HDR capability; the 1,000-nit default is not display detection.
3. Start at **Paper white = 200 nits**, **Highlight strength = 0.5**, and
   **Source gamma = 2.2**. Adjust paper white for the room. Gamma 0 uses the
   piecewise sRGB curve instead of a power curve.
4. **Compare SDR / HDR** puts SDR at paper white on the left and expanded HDR
   on the right. The three-pixel white line marks the split. Both halves are
   presented through the HDR swapchain.
5. Calibration shows a strip with near-black steps, paper white/gray steps,
   or four levels up to the chosen display peak. The strip ignores the guest
   HUD mask and comparison split. Turn calibration and comparison off to play.

Launcher changes take effect on the next game launch. The SDK cvars also support
hot reload for hosts that expose it. Equivalent `forza.toml` keys:

```toml
present_hdr = true
present_hdr_paper_white_nits = 200.0
present_hdr_peak_nits = 400.0 # example for a 400-nit display
present_hdr_highlights = 0.5
present_hdr_gamma = 2.2
present_hdr_compare = false
present_hdr_test_pattern = 0 # 1 near black, 2 paper white, 3 peak
```

At strength 0, the scene stays within paper white. At strength 1, SDR white in
the scene reaches the selected peak. At intermediate strengths the maximum is
`paper_white + (max(peak, paper_white) - paper_white) * strength`.
Peak at or below paper white disables expansion. Invalid non-finite numeric
values fall back to defaults; finite values are clamped to the displayed ranges.

## HUD protection and implementation

SDK patch 0075 adapts the scRGB presenter, highlight curve, and coverage shaders
from [PinyonShift's SDK fork](https://github.com/arcanite24/shiftglue-sdk), notably
commits `36275a6`, `2c82d4d`, and `159464d` (BSD-3-Clause). It keeps this project's
pinned SDK and uses its existing render-target cache rather than importing the
fork's native command executor. The original copyright/license notices remain.

The renderer snapshots the full-size 10-bit scene at the game's scene-to-HUD
format transition, after any native-HUD scene seeding, before the first HUD draw.
Three snapshots follow the existing frame submission retirement rules. Comparing
the final picture with that snapshot produces conservative HUD coverage in the
guest output's alpha; a one-pixel dilation covers antialiased edges. The scRGB
pass expands only scene pixels. Host UI also marks its coverage.

This is a visual difference mask, not game-provided HUD geometry. Translucent
overlays may suppress highlights below them. A missing/current-size-mismatched
snapshot or unavailable compute-mask path retains paper-white output instead of
expanding an unprotected HUD. Scene expansion currently requires the ordinary
host-render-target Vulkan path and FH1's 1280-pitch scene transition. Other
render-target paths, unusual guest modes, and ultrawide need separate validation.

HDR attachments and descriptors retire after rendering and queued presentation
have finished. Presentation semaphores belong to swapchain images, so an image's
next acquisition establishes that its previous presentation wait has completed.
The queue idle wait is limited to swapchain retirement.

## Verification

An SDR guest screenshot proves scene/HUD placement, not HDR luminance. For a
numerical capture of the actual presented float attachment, start a developer
build with `--present_hdr_capture=/absolute/path/hdr.trigger`, then create that
file after entering gameplay. It is consumed once, writing:

- `.rgba16f`: tightly packed, little-endian RGBA16 float, linear scRGB.
- `.bgra8`: the same frame's SDR stand-in, with UI coverage in alpha.
- `.json`: physical swapchain size and HDR settings.

The diagnostic is off by default. It stalls for a readback only when triggered,
requires transfer-source support on the surface, and does not upload anything.
Run `python tools/check_hdr_capture.py /absolute/path/hdr.trigger --expect-scene`
(NumPy required) to check finite values, the output curve, HUD brightness and
scene highlight expansion. Keep these binary captures outside the repository.
Only screenshots, text reports and logs belong in a PR.

Physical display brightness and tone mapping remain a display/OS check. Linux
lavapipe/X11 can validate fallback and rendering, but cannot validate a physical
HDR desktop or an AMD driver's HDR behavior.

### Local results, 2026-10-18

- Release SDK runtime/GPU and TU4 game builds passed on Windows and Linux. The
  full 64-file patch series applies cleanly to `bd833a2`; its resulting sources
  match those used for both SDK builds.
- Launcher suite: Windows 197 discovered (23 platform/optional skips), Linux
  197 discovered (9 skips), no failures. The four new HDR tests run on both,
  including the Qt backend/toggle dependencies.
- Windows: NVIDIA RTX 5060 Ti, driver 617.14, Vulkan 1.4.351; Windows HDR enabled.
  DXGI reports a 400-nit display peak. Guest 2560x1440 at 2x render scale,
  physical scRGB swapchain 3200x1800 at 125% desktop scaling. Menu and gameplay
  renders, plus 720p/1x tests, use copied saves.

| GPU attachment test | Scene maximum channel, scRGB x 80 | Opaque HUD maximum channel |
| --- | --- | --- |
| Paper white 200, peak 400, strength 0.5, gamma 2.2 | 299.06 nits | 200 nits |
| Paper white 200, peak 400, strength 1, gamma 0 | 398.13 nits | 200 nits |
| Paper white 200, peak 400, strength 0, gamma 2.2 | 200 nits | 200 nits |

All three readbacks are finite and pass `check_hdr_capture.py`; the two
expanded gameplay captures contain both protected HUD and scene pixels above
paper white. These are numerical signal levels, **not photometer measurements**.
The peak calibration strip reads back exactly 100/200/300/400 nits. HDR off
selects format 44 / color space 0; HDR on selects format 97 / color space
1000104002. After correcting snapshot transfer usage, Windows validation and
synchronization validation report no errors in these runs. The existing
unused fragment-output warning is still emitted.

Linux: Fedora WSL, lavapipe/LLVM 22.1.8, Xvfb/X11, 1280x720. With HDR requested,
the presenter logs that the surface does not offer HDR and creates format 44 /
color space 0. The game boots, renders its menus and reaches free roam through
the fallback. No
physical Linux HDR display, AMD HDR driver, DLSS/FSR combination, or ultrawide
HDR validation is claimed by these tests.

The Windows test also signed into an isolated loopback Forza Live server and
received HTTP 200 title-service/profile responses with HDR active. This does
not establish a multi-machine Horizon World session or public-server routing.

## Windows online setup

Windows uses the same launcher and Forza Live protocol as Linux. On a real
Vulkan GPU, install `ReImagined-Setup-*.exe`, choose the complete extracted game
folder under **Setup > Game files > Choose folder**, then build. The launcher
reuses that folder; choosing it does not require another extraction or a 7.6 GB
copy. Both facade files and the `media` folder must accompany `default.xex`.
See [launcher setup](launcher.md) for supported executable checks and TU4 setup.

Under **Settings > Online**, choose the public server, local server, or a custom
`host:port`. For a self-hosted server on Windows, from the repository root:

```powershell
py -3 server/forza_live_server.py --host 0.0.0.0 --http-port 21201
```

Allow UDP 21200 and TCP 21201 for other machines to reach it; use
`127.0.0.1:21200` for an isolated local test. The launcher can start its local
server automatically. Horizon World is **Multiplayer > Find Game > Horizon
World**; all players need matching base/TU4 versions. HDR and online are
independent. See [hosting](hosting.md) for routing and server configuration.
