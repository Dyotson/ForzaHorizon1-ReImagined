# VR: feasibility study and head-tracking prototype (#333)

Goal: sit in the car with a headset (OpenXR: SteamVR, Quest Link, Monado), as in Assetto
Corsa. This is step 1: where the game's camera can take a head pose, what stereo would take
and cost, where the HUD and menus are drawn, and a head-tracking prototype without a
headset. Addresses are the disc executable's, with Title Update 4's in brackets.

## Summary

- **The camera is easy to reach.** Each player view (a `CSplitView`) has a *camera
  director* that holds the active camera and, per frame, the finished view matrix
  (director + 0xF0, world to camera). Exactly two functions read that matrix: the one that
  hands it to the renderer's presentation camera and the one that builds the game-side view
  frustum. Putting a head pose into that matrix for the duration of those two calls turns
  the view inside the car with everything else (shadows, mirrors, culling) following, and
  leaves the camera's own state untouched. The prototype does exactly that.
- **The in-car view is `Camera::CCamDriver`**, not `CCamCockpit` (which the player never
  uses): driver's eye 0.21 m above and 0.51 m behind the steering wheel, 62 degree vertical
  FOV, near plane 0.1 m (`media/camera.zip` > `CameraSettings.ini`, `Driver\...`).
- **Drawing the scene twice has an engine path, but it costs CPU where there is none to
  spare.** The engine keeps Forza Motorsport 4's two-player split-screen: the world
  update loops over two `CSplitView`s, each with its own director, presentation camera and
  viewport, and a split-screen render scenario cuts crowd, particles, reflections and MSAA
  tiling to afford it. Whether FH1's menus still reach it is not established. A second eye would add the scene's share of the command processor and the
  render thread (the scene is about two thirds of the ~3,000 host draws per frame, measured
  with RenderDoc), roughly +5 to +8 ms of CPU per frame on a 5 GHz Zen 4, on the two threads
  every frame waits for; simulation, physics, audio and the HUD stay single. Fine as a
  correct-stereo reference on fast PCs, not as the product. A lead: the scene is drawn in
  three predicated EDRAM tiles, so the emulated GPU processes it three times; a non-tiled
  scene would save more command-processor time than a second eye costs.
- **The HUD is already separable:** the SDK (patch 0046) finds the frame's scene/UI split
  from the front buffer's formats and can draw the UI into its own target, which a
  cockpit-fixed or head-locked panel needs.
- **Built: an OpenXR session with a virtual screen and a head-tracked mono cockpit**
  (`vr_mode`, launcher Display > VR (experimental) > VR headset (experimental)); [what it
  does, how, and the test steps for a Quest 3 with WiVRn](#openxr-session-and-cockpit-milestones-1-and-2).
  Version 2, after the first headset test: camera physics off in the cockpit, the field of view
  fitted to the eyes, pads without focus, a smaller screen, the trace
  ([what changed](#version-2-after-the-first-headset-test)).
- **Recommended plan** (with the integration research, merged below): (a) an OpenXR session
  with a virtual screen, then the head-tracked mono cockpit (the prototype's hook fed by
  the headset, the HUD on a quad, the game at its own rate under the runtime's
  reprojection); (b) stereo by warping that view to both eyes with the depth the DLSS/FSR 3
  path already decodes, with the split-screen view as a correct-stereo reference, and true
  stereo inside the GPU emulation last; (c) comfort (camera effects, motion blur and depth
  of field off, recenter, IPD) and the UI on panels. Sizes and order below.

## The camera system

### Where the view is computed

Per frame, on the main loop's thread (`FORZA_VR_POSE_TRACE=1` logged the push on one thread,
and the frustum's chain starts in the same world update):

```
world update                      sub_823E3218 (TU4 824131A8)
  for each of 2 split views       manager + 244 + 172 * i   (i < 2, a fixed loop)
    CSplitView::Update            sub_825AA1B0 (826223B0)
      director update             sub_82858288 (82908710): active camera -> view matrix
      push to the renderer        sub_825A9D20 (82621F20)
      ...
  game-side visibility            sub_829A7448 (82A5B920) -> sub_829A6E18 -> sub_829A5598
                                  -> sub_829A4860 (82A58DA0): view 0's frustum
                                     (sub_823EAED0 builds it from matrix, FOV, aspect, clip)
```

The manager is `*(*(*0x832DF024 + 4) + 4)` (TU4 `0x833CAAE4`), the same object whose
+0x80/+0x84 is the vehicle list (`tools/autodrive.py`).

**`CSplitView`** (vtable `0x82029F38` in TU4): +8 the camera director, +12 the viewport
(x, y, width, height as integers: 0, 0, 1280, 720; then min/max Z), +40 the view's
presentation camera (a deferred `IPresentationCamera`). In single player view 1's director
is null.

**The camera director** (no RTTI; read from guest memory in free roam and in a race):

| Offset | What |
| --- | --- |
| +0 | the `Forza2::CPlayer` |
| +20 | the active camera (a `Camera::ICamera`) |
| +24 ... +116 | the player's cameras: FollowLow, FollowHigh, Driver, Hood, BumperHigh, three Free, FreeTargetCar, FixedFree, Wheel, Side, Ortho, four TSC (track-side), 321, PreRaceTrack, FreeTrack, PhotoMode, RearView, Blend, NoCar |
| +176 | camera position in the world (x, y, z, 1) |
| +208, +224 | camera forward and up in the world |
| +240 | **the view matrix**: 4x4 floats, world to camera, Direct3D row-vector convention, left-handed camera space (x right, y up, z forward). Rows 0-2 hold the camera's right, up and forward axes as columns, row 3 is `-position . axis` for each axis |
| +304 | vertical field of view in radians (1.0821 = 62 degrees in the cockpit, 1.0123 = 58 chase) |

`sub_8284EF88` (`828FF258`) returns director + 240 and `sub_8284EF90` the FOV; near and far
come from the active camera's virtual functions at +32 and +36. The only callers of the
matrix accessor are `sub_825A9D20` (twice) and `sub_829A4860`.

**The cameras.** The car-relative ones (Cockpit, Hood, Driver, BumperHigh, RearView) share
their update `sub_8285C8C8` (`8290CD80`); Wheel and Side have their own. vtables (RTTI):
CCamDriver `0x82075DD4` (`0x820860FC`), CCamCockpit `0x82075CD4` (`0x82085FFC`), CCamHood
`0x82075D54`, CCamRearView `0x82075FD4`. The right stick's look-around is a yaw angle at
camera + 280; `sub_82851518` turns the car's forward axis by it. With that look turned 90
degrees the game draws the view without the car (no door, no pillar), so the game's own
look-around is not a head pose: it changes what is drawn.

`RB` changes the camera (FollowHigh -> Driver -> ...); `BACK` opens the map.

### Where the matrices end up

`sub_825A9D20` (view, flag) calls the view's presentation camera (a
`CPresentationCameraDeferred`, game thread) with:

| vtable offset | Call | Argument |
| --- | --- | --- |
| +168 | SetViewTransform (stub `sub_82DB9AE8`) | the director's view matrix, by value |
| +56 | SetFOV | FOV x a constant |
| +68 | SetAspectRatio | `sub_823E22C8` (the viewport query `hooks/ultrawide_projection.cpp` widens) |
| +196 | SetZClip | near, far |
| +80, +44, +236, ... | further setters with values from the active camera (its virtual functions +76, +80, ...), not identified | |

and then hands the same set (matrix, FOV, aspect, near, far, a vector from the director and
two flags from the active camera) to `sub_82DBADC0`. The deferred stubs queue each call (80 bytes for a matrix) for the render
thread (`sub_8261E178`), where `CPresentationCamera::SetViewTransform` (`sub_823EB7A0`)
stores the matrix at camera + 16 and sets the dirty flag at + 464; the projection
(SetProjectionTransform, +172, `sub_82D8C820`) goes to + 80. The renderer builds the
per-object constants from there on the CPU: the car shaders take `matWVP`, `matWV`, `matVP`,
`matVInv` (`media/shaders/Cars/*.fxobj`), the engine's world shaders `CamViewProjMat`,
`CamProjMat`, `PCamViewProjMat` (strings in `default.xex`). So the GPU never sees the camera
alone: per object it gets world-view-projection products computed by the render thread.

### Where to apply a head pose

A head pose is a rotation R and an offset t of the eye in the camera's own space. With the
row-vector view matrix V, the posed view is `V * inverse(H)`, `H = [R 0; t 1]`
(`src/platform/vr_head_pose_math.h`). The cleanest point is the director's matrix between
the director update and its two readers:

- the renderer gets the posed view, so the main view, its culling and the shadows follow
  (seen in the screenshots), and so does everything else the render thread derives from the
  presentation camera, such as the motion-blur reprojection;
- the game-side frustum follows too (`sub_829A4860`), so game-side visibility uses the same
  view;
- the camera's own state (look-around angle, springs, smoothing) never sees the pose, so
  nothing integrates it and nothing drifts, and switching cameras or pausing is unaffected.

In the car frame: the cockpit camera is rigidly attached to the car (plus the game's camera
shake and G-force head movement, `CameraPhysics.xml` > `CameraSpaceYPROffset` /
`CameraSpaceXYZOffset`), so camera space is the car frame up to those effects. A headset's
pose relative to its calibrated seated origin goes in as R and t directly; the effects
should be off in VR (stage c).

For stereo the same point takes the eye offset: t.x = -IPD/2 for the left eye, +IPD/2 for
the right (the second view would be the second split-screen view, or the GPU emulation's
second eye). The projection must become the headset's asymmetric per-eye frustum; the
presentation camera has a full projection matrix setter (+172) besides SetFOV/aspect, but
which one the renderer prefers is not checked yet.

## Head-pose prototype

`src/hooks/vr_head_pose.cpp`, off by default (cvar `vr_head_pose_test`, category VR).
With the cvar off the hooks call the original right away; normal play is unchanged.

```
--vr_head_pose_test=true
FORZA_VR_POSE="yaw,pitch,roll,x,y,z"   fixed pose from the start (degrees, metres)
FORZA_VR_POSE_FIFO=<path>             mkfifo it; each line "yaw pitch roll x y z" replaces
                                      the pose ("skip view|frustum|none" for A/B tests)
FORZA_VR_POSE_ALL=1                   every camera, not only the in-car ones
FORZA_VR_POSE_TRACE=1                 log the calling thread every 300 calls
```

Positive yaw turns the head left, positive pitch looks up, positive roll tilts the head
left; x is right, y up, z forward in metres. The pose applies while the active camera is
CCamDriver or CCamCockpit. Both readers of the matrix are hooked (`sub_825A9D20` and
`sub_829A4860`): each writes the posed matrix into the director, runs the original and
writes the game's matrix back unless the game already wrote a new one. The math has a
standalone test (`forza-recomp/tests/vr_head_pose_math_test.cpp`: yaw, pitch, roll,
offset and their order).

Example (a test session as in `tools/live_session.sh`, scratch save and data dirs):

```sh
mkfifo /tmp/pose.fifo
FORZA_VR_POSE_FIFO=/tmp/pose.fifo tools/live_session.sh DIR 600 --vr_head_pose_test=true ...
echo "60 0 0 0 0 0" > /tmp/pose.fifo     # look left
echo "0 30 0 0 0 0" > /tmp/pose.fifo     # look up
echo "15 -5 10 0.2 0.05 0.05" > /tmp/pose.fifo   # lean right and tilt
```

### Results (TU4, Linux, RX 9060 XT, 720p, 30 fps)

Three runs on a scratch copy of the save: free roam standing at the spawn (straight, 60
degrees left and right, 30 up, 150 back, a lean of 0.25 m right with yaw/pitch/roll), and the
GoPro Millennia Masters festival race driven by `tools/autodrive.py` at 90-140 km/h in the
cockpit view (straight, left, right, up, lean, 75 left and right, 160 back), poses sent
through the FIFO while driving. Screenshots stay out of Git.

Works:

- The view turns and moves inside the car: door, pillars, roof lining, seats, the driver's
  arms and hands, the dashboard and the gauges are all drawn from the new eye point, at any
  angle tried. The game's own look-around (right stick) drops the car at 90 degrees; the
  posed view keeps it.
- Nothing visibly culled or popping at 60-160 degrees while driving: houses, trees, the
  crowd, guard rails, other cars, the shadows. The render side culls with the presentation
  camera, which gets the posed matrix.
- The rear-view and wing mirrors keep working (they have their own camera, CCamRearView).
- 30 fps held, no fault in the log; the camera's state is never modified, so switching
  cameras and the race intro behave as before.

Off, or to handle later:

- The HUD stays fixed on the screen (expected: it is a 2D overlay, see below).
- Only arms and hands exist: looking down or back shows the seat without a body.
- The mirrors show the unposed mirror image (their camera does not follow the head;
  physically the image should shift with the eye).
- The game's motion blur reprojects with the previous frame's camera, so quick head turns
  would smear; in VR it has to be off.
- The game-side frustum (`sub_829A4860`) shows no visible effect: at a near standstill,
  shots at 75, -75 and 160 degrees with that reader posed and unposed (`skip frustum`) differ
  no more than two shots with the same setting (mean pixel difference 30.4 against a 31.5
  noise floor, 4.7 against 6.0, 6.8 against 7.3), while `skip view` alone shows the unposed
  forward view. It feeds something the screenshots do not show (spawning, streaming
  priority or sound are candidates); posing it is harmless and keeps it consistent.
- Not tried: a full lap with continuous head motion, night (headlights), rain, other cars'
  interiors, the photo mode and replays (FORZA_VR_POSE_ALL applies to every camera).

## OpenXR session and cockpit (milestones 1 and 2)

Built on top of the study (plan step (a)1 and the head-tracking part of (a)2). Code:
`src/platform/xr.{h,cpp}` (loader, instance, session, the OpenXR thread),
`src/platform/xr_math.h` (poses and fields of view), `src/platform/xr_frame_tags.h` (frame
pairing), the camera hooks in `src/hooks/vr_head_pose.cpp` (and the viewport query's in
`src/hooks/ultrawide_projection.cpp`), SDK patches 0390, 0391 and 0392
([sdk_patches.md](sdk_patches.md)), the launcher's `forza_launcher/vr.py`. Version 2 (below,
[after the first headset test](#version-2-after-the-first-headset-test)) changed the cockpit's
field of view, the camera physics, pad focus, the screen size and the latency estimate. The OpenXR headers
are vendored (`thirdparty/openxr`, OpenXR-SDK 1.1.62, Apache-2.0 or MIT); the loader is opened at
run time, never linked.

### What it does

| Setting (cvar) | Default | What |
| --- | --- | --- |
| VR headset (experimental) (`vr_mode`) | `off` | `off`: nothing VR runs, no OpenXR library is loaded. `screen`: virtual screen. `cockpit`: virtual screen plus the head-tracked in-car view |
| Seat height (cm) (`vr_seat_height_cm`) | 0 | Cockpit: raises or lowers the eye from the driver's position |
| `vr_recenter_key` | `Home` | Recentres the view (both sticks pressed for a second do the same) |
| `vr_fov_margin` | 5 | Degrees drawn beyond the headset's field of view on each side |
| `vr_camera_effects` | false | Cockpit: keep the game's camera physics (shake, G-force lean and tilt, speed field of view) on top of the head pose |
| `vr_head_back_limit_cm` | 8 | Cockpit: how far the eye may move back from where it was recentred (100: no limit) |
| `vr_screen_width`, `vr_screen_distance` | 2.2, 2.5 | The virtual screen in metres (47 degrees wide; version 1: 3.2 m, 65 degrees) |
| `vr_runtime_timeout` | 20 | Seconds to wait for the runtime at start |
| `vr_trace` | false | Logs the head pose, the game camera, the field of view and the latency about 10 times a second ([the trace](#the-trace-vr_trace)) |

- **Virtual screen** (both modes): the newest game picture on an OpenXR quad layer 2.2 m wide
  (height from the picture's aspect) 2.5 m in front of the recentred head, at its height, in
  `LOCAL` space, redrawn at the headset's rate whatever the game's frame rate. A picture drawn
  at render scale 2 or more is halved (or more) with a linear filter on the way: the runtime
  samples layers without mipmaps, and the screen is about 1,000 headset pixels wide. The desktop
  window keeps showing the game as before (it is the mirror).
- **Cockpit:** while the active camera is the in-car one (`CCamDriver`, or `CCamCockpit`), the
  camera hook takes the head pose from the headset instead of `FORZA_VR_POSE`: rotation and
  position relative to the recentred head (held at most `vr_head_back_limit_cm` behind it),
  plus the seat height. The game draws exactly the union of both eyes' fields of view plus the
  margin: around an axis pitched to the middle of the eyes' up and down angles, with a
  projection aspect of its own (Quest 3: 123 x 109 degrees, the axis 5.5 degrees down), the
  vertical field of view written into the director's FOV (+304) and the aspect returned by the
  viewport query, for both readers, like the matrix. The game's camera physics are off for that
  camera. That picture goes to a projection layer with the pose and field of view it was drawn
  with, the same image for both eyes (mono, no depth); the runtime reprojects head motion
  between game frames. Other cameras, menus, loading and cut scenes show on the virtual
  screen.
- **HUD:** it stays in the game's picture. In the cockpit picture the speedometer and map sit in
  its corners, 48-59 degrees to the side: each at the outer edge of one eye only ([version
  2](#hud-and-menus-c)); the car's own gauges are in the cockpit (`cockpit_gauges_lit` keeps them
  lit). A HUD on its own quad needs the scene/HUD split per frame (SDK patch B of the plan): not
  done here.
- **Recentre:** at the session's first tracked frame, with `vr_recenter_key` (Home), both sticks
  held for a second, and when the runtime recentres its own space (Quest: hold the Meta button).
  It keeps the heading only, so the horizon stays level.
- **Failures** log one line that starts with `VR:` and ends with `- playing flat`: no loader
  (`libopenxr_loader.so.1` / `openxr_loader.dll`), no runtime installed or active, the runtime
  did not start (its server not running), no answer within `vr_runtime_timeout` (WiVRn's
  `xrCreateInstance` waits until a headset connects, so the probe runs on its own thread), no
  headset, a runtime without `XR_KHR_vulkan_enable2`, an SDK without patches 0390/0391, a
  renderer other than Vulkan, a Vulkan device not created through the runtime, a failed session,
  a lost session (the game goes on flat from there).
- **Pads without focus:** while the session runs, pads and wheels keep working when the desktop
  window has no focus (patch 0392); the keyboard still needs the focus.
- Not done: stereo, the HUD quad, motion blur and depth of field (still on: no clean switch
  found yet, below), the guest vblank locked to the headset (patch C), D3D12.

### How it works

1. `ForzaApp::OnPreSetup`, before the GPU plugin creates the Vulkan provider: `xr::Prepare` opens
   the loader and, on a thread it waits for up to `vr_runtime_timeout`, creates the instance
   (`XR_KHR_vulkan_enable2`, `XR_KHR_convert_timespec_time` or the Win32 counterpart), finds the
   head-mounted display and reads its Vulkan requirements. Then it registers the Vulkan creation
   hooks (patch 0390): the runtime creates the `VkInstance` (`xrCreateVulkanInstanceKHR`), picks
   the physical device (`xrGetVulkanGraphicsDevice2KHR`) and creates the `VkDevice`
   (`xrCreateVulkanDeviceKHR`), each with the extensions it needs on top of the SDK's.
2. `OnCreateDialogs` (the provider exists): `xr::Start` checks that the device is the one the
   runtime made, turns on the guest output export (patch 0391) and starts the "Forza OpenXR"
   thread: session on graphics queue 0 of the game's device, `LOCAL` and `VIEW` spaces, an 8-bit
   sRGB swapchain the size of the guest output, then the frame loop: `xrWaitFrame`,
   `xrBeginFrame`, the newest guest output if there is a new one, `xrEndFrame`. The device's queue
   lock (`AcquireQueue`) is held around `xrBeginFrame`, `xrEndFrame`, swapchain acquire and
   release and the copy's submission, the calls `XR_KHR_vulkan_enable2` says use the queue.
3. The copy: the guest output (A2B10G10R10, gamma-encoded) is blitted to an 8-bit UNORM image and
   copied bit for bit into the sRGB swapchain image (a blit into sRGB would encode the gamma a
   second time). Barriers as the presenter's own guest output capture: the guest output leaves and
   returns to `SHADER_READ_ONLY` in the fragment shader stage, so a later refresh of the same
   image waits for the copy (same queue). The frame is held (patch 0391) until the copy's fence.
4. Frame pairing (`xr_frame_tags.h`): the view hook (main loop thread) asks for the head pose
   predicted for now plus the measured latency (camera built to picture shown, an average of 5 to
   100 ms), poses the matrix and remembers its 64 bytes with a new tag. On the render thread the
   hook on `CPresentationCamera::SetViewTransform` (`sub_823EB7A0`, TU4 `sub_8241B860`; the matrix
   lands at camera + 16) finds the tag by the bytes; VdSwap writes the frame's tag into the swap
   packet (one more dword in `PM4_XE_SWAP`, which a stock command processor skips); the command
   processor hands it to the presenter with the guest output (patch 0391); the OpenXR thread looks
   the pose and field of view up. The render thread also swaps frames without a new view (it draws
   the camera's last view again), so the tag stays with the render-side camera until it gets an
   unposed matrix (another camera). A tag more than 300 ms old at display time means the game
   stopped building in-car views: the picture goes back on the screen.
5. Every 10 s a line `VR: N Hz headset, N game frames/s copied; shown as cockpit N / screen N
   frames; swaps tagged N/N (dropped views N, repeated N); latency N ms (game N + runtime N;
   predicting N); xrEndFrame max N ms`. Dropped views: the render thread skipped a main loop
   frame; repeated: it drew the last one again. Latency is measured on cockpit frames only
   (`latency -` without any).

### Tested without a headset (Linux, TU4, RX 9060 XT, RADV)

- **VR off:** no OpenXR library mapped, no `VR:` line, frame rate as before (the off paths are
  one relaxed atomic load per camera call, swap and guest output refresh).
- **No runtime** (`vr_mode=cockpit`, no `active_runtime.json`): `VR: no OpenXR runtime is
  installed or active (XR_ERROR_RUNTIME_UNAVAILABLE) ... - playing flat`, the loader unloaded.
- **WiVRn selected (`XR_RUNTIME_JSON` = the Flatpak's `openxr_wivrn.json`), server not running:**
  WiVRn's client cannot connect to `$XDG_RUNTIME_DIR/wivrn/comp_ipc`; `xrCreateInstance` fails:
  `VR: the OpenXR runtime did not start (XR_ERROR_RUNTIME_UNAVAILABLE) ... - playing flat`.
- **WiVRn server running (user space: `flatpak run --command=wivrn-server io.github.wivrn.wivrn
  --no-manage-active-runtime --no-publish-service`), no headset:** WiVRn's `xrCreateInstance`
  blocks until a headset connects (the first build hung at start). Now: `VR: waiting up to 20 s
  for the OpenXR runtime`, then `VR: the OpenXR runtime did not answer in 20 s (WiVRn waits until
  the headset connects ...) - playing flat`; the game starts flat and exits cleanly.
- **A mock OpenXR runtime** (a scratch test library, not in the repository: `XR_KHR_vulkan_enable2`
  that adds instance and device extensions as Monado does, a 90 Hz frame clock, a head that
  turns 25 degrees left and right every 6 s, checks of every call and layer, dumps of the shown
  image): screen mode put a 3.2 x 1.8 m quad at 2.5 m with the game's picture (colours as on the
  desktop, so the gamma is right); in cockpit mode (free roam, the in-car view) the window showed
  the view turning with the simulated head inside the car at 114 x 140 degrees, the runtime got
  projection layers with that field of view, all swaps were tagged in 10 s windows spent in the
  car (281/281, 278/278), the submitted pose was within 0.1 degree of the head at display (the
  mock's prediction is exact; a real runtime's is not), switching to the hood camera went back to
  the screen, both sticks recentred, a session loss and no headset fell back to flat, and closing
  the game ended the session (`xrRequestExitSession`, stopping, idle, exiting, destroyed). The
  mock's checks found no error. The game held its 28-30 fps; `xrEndFrame max` read about 230 ms
  there because the mock waits for the GPU while it dumps a picture every 3 s.
- **Packaging:** the loader built as `tools/build_rexglue_sdk.sh` builds it (OpenXR-SDK 1.1.62)
  works with the game; in a freedesktop 26.08 Flatpak sandbox with the manifest's new permissions
  that loader found the WiVRn Flatpak's runtime and connected to its server (`xrCreateInstance`
  waiting for the headset); without `xdg-run/wivrn` the connection fails.
- **Unit tests:** `tests/xr_math_test.cpp` (OpenXR to game axes against the test pose's yaw,
  pitch and roll, recentring, seat height, the covering field of view, the latency estimate),
  `tests/xr_frame_tags_test.cpp` (tags, matrix matching, the swap tagger, layer choice, the swap
  packet), `tests/vr_head_pose_math_test.cpp`; all three run in CI.
- **Windows:** the game's and the patched SDK's changed sources compile for Windows (syntax
  check with the cross toolchain); not run.

Only the headset can tell: WiVRn creating the Vulkan instance and device for the game's device
and its session starting; the screen's size and sharpness; the colours; smoothness at the
headset's rate with the game at 30 fps; whether holding the queue lock in `xrEndFrame` stalls the
game (`xrEndFrame max` in the 10 s line); how far the head prediction is off and whether the
5 degree margin hides the picture's edge when the head turns; the HUD's place; comfort.

### Packaging

- **Linux package and Flatpak:** `tools/build_rexglue_sdk.sh` builds `libopenxr_loader.so.1` from
  OpenXR-SDK 1.1.62 (pinned SHA-256) into the overlay's `lib/`, `tools/package_sdk_overlay.sh`
  packs it, and the launcher copies it next to the game (`builder.stage_runtime`;
  `tools/package_linux.sh` for a hand-made package). The game's `LD_LIBRARY_PATH` holds the game
  folder, so it is found first; without it the system's loader is used (Fedora `openxr-libs`,
  Ubuntu/Debian `libopenxr-loader1`, Arch `openxr`). The freedesktop 26.08 runtime has none.
- **Flatpak permissions** (manifest): `xdg-config/openxr:ro` (the active runtime manifest),
  `/var/lib/flatpak/app/io.github.wivrn.wivrn:ro` (the runtime library of a system-wide WiVRn
  Flatpak; a user install is under the home folder, already readable), `xdg-run/wivrn:ro` and
  `xdg-run/monado_comp_ipc` (the compositor sockets). Inside the sandbox `XDG_CONFIG_HOME` is the
  app's own folder, so the launcher sets `XR_RUNTIME_JSON` to the host's
  `~/.config/openxr/1/active_runtime.json`. Runtimes installed on the host in `/usr` (Monado,
  WiVRn from distribution packages) cannot be loaded in the sandbox (its `/usr` is the runtime's);
  SteamVR's Linux runtime lives under the home folder but was not tried.
- **Windows:** no runtime installs the loader; `openxr_loader.dll` must sit next to `forza.exe`.
  The release workflow's sdk-windows job (and `tools/build_windows.sh`) puts it into the
  overlay's `bin/` from `openxr_loader_windows-1.1.62.zip` of the KhronosGroup/OpenXR-SDK-Source
  release (pinned in `launcher/forza_launcher/pins.py` as `openxr-loader-windows`, SHA-256
  `800ec772e2f9448a26ab9f579f4914d984346dd9d0d7c007841abe21d2c8ff2f`, `x64/bin/openxr_loader.dll`,
  Apache-2.0, its licence in `share/licenses/openxr-loader/`), and the launcher copies every DLL
  of that `bin/` next to `forza.exe`, so the installer's build carries it. The game build's
  `licenses/openxr-sdk.txt` covers it. The launcher forces `--gpu_backend=vulkan` when the
  setting is on. Built in CI only: not run with a headset (SteamVR, Meta Quest Link, Virtual
  Desktop) yet.

## Testing on a Quest 3 with WiVRn

1. **Build** this branch: the SDK overlay with patches 0390, 0391 and 0392
   (`tools/build_rexglue_sdk.sh`, which also builds the OpenXR loader) and the game as usual, or
   let the launcher build it. `libopenxr_loader.so.1` is also installed on this PC
   (`/usr/lib64`), so the loader is there either way.
2. **Start WiVRn** on the PC (the WiVRn app from the Flatpak `io.github.wivrn.wivrn`), put the
   Quest 3 on, open WiVRn on the Quest and connect to the PC. Wait until the Quest shows that it
   is connected and waiting for an application. While it runs, WiVRn makes itself the active
   OpenXR runtime (`~/.config/openxr/1/active_runtime.json`). Connect first, then start the
   game: WiVRn makes the game wait until a headset is there (20 s, then the game starts flat).
3. **Launcher:** Settings > Display > VR (experimental) > **VR headset (experimental)**:
   **Virtual screen**. Press Play. For the trace, add `--vr_trace=true` to the extra arguments.
4. **What you should see:** the game in its window on the desktop as always, and in the headset a
   screen 2.2 m wide 2.5 m in front of you at eye height (47 degrees: the whole picture in view
   without turning the head) with the game on it, smooth when you move your head. Press **Home**
   on the keyboard, or hold **both sticks pressed** for a second, to bring the screen in front of
   you again. The pad works while you wear the headset, whichever window has the focus.
5. **Cockpit:** close the game, set **VR headset (experimental)** to **Cockpit**, Play, sit as
   you would drive. Load into free roam, press **Home** (or hold both sticks) once while looking
   straight ahead, then switch to the in-car view (**RB** cycles the cameras; the one with the
   driver's hands on the wheel). You should be inside the car: turning and moving your head
   looks around the interior (door, pillars, mirrors, the roof); the world outside updates at
   the game's frame rate while the head motion stays smooth. The car's interior stays put at any
   speed: no shake, no lean in corners, no zoom at speed. Both eyes see the same picture, so
   there is no depth: the cockpit looks large and flat. Turned round you see your seat from the
   front (the driver has no body), from at most 8 cm behind where you recentred. The speed and
   map HUD sit at the outer edges of the view (hide them in the game's HUD options if they
   bother you). Other cameras, the map and menus are on the screen. **Seat height (cm)** moves
   the eye up or down; recentre after changing it. Try a drive.
6. **Send back** `forza.log` (launcher: Play page > Open log folder; by default
   `~/.local/share/forza/forza.log`), or at least `grep -n "VR\|Vulkan device\|Input: pads\|rror"
   forza.log`. The useful lines: `VR: OpenXR runtime WiVRn ...`, `VR: headset ...`, `Vulkan
   device chosen by the creation hook (OpenXR runtime): ...`, `VR: session created ...`, `Input:
   pads and wheels work while the window has no focus`, `VR: session focused`, `VR: eye fields of
   view ...`, `VR: the cockpit view is drawn N deg wide x N tall ... N x N pixels per degree in
   the middle`, `VR: swapchain ...`, `VR: the in-car camera's camera physics are off ...`, the
   `VR: N Hz headset, ...` line every 10 s (in the car: cockpit frames, swaps tagged N/N, latency
   with its game and runtime parts, xrEndFrame max), the `VR trace:` lines, and a `... - playing
   flat` line if VR gave up. If the session did not start, WiVRn's own log (the WiVRn app) too.
   And what you saw: the screen's size and sharpness; the cockpit's sharpness; stutter; whether
   anything still moves the view besides your head; what you see when you look back; black edges
   when turning the head quickly; where the HUD was.

## Version 2: after the first headset test

The first test (Quest 3 over WiVRn, cockpit, render scale 2, 72 fps, 25 October 2026) worked
first time but was not playable. What was reported, what caused it and what changed:

| Report | Cause | Change |
| --- | --- | --- |
| h. Camera shake at high speed, very nauseating | The in-car camera's `CameraPhysics.xml` layers (`DriverCam`, 51 layers): Simplex and Perlin noise on the camera's yaw, pitch, roll and position driven by `SpeedMPH`, collision jolts, surface bumps | Camera physics off for the head-posed camera (below) |
| i. The camera "changes angles a lot" | The same layers: G-force lean and tilt (`Gs_Lateral` turns the view 0.35 degrees per g toward the corner and rolls it 1.3, `Gs_Longitudinal` pitches it and moves the eye), slip, a constant 1.1 degree pitch, and 8 degrees more field of view at speed. All of it on top of the head pose | Same |
| a. The pad stops working once in the headset | The SDK gives a neutral pad while the window lacks focus (`ReXApp`'s input active callback); the desktop window loses it when the player looks at the headset (`Window lost focus` in the log) | SDK patch 0392: pads and wheels work without focus while the session runs; the keyboard stays focus-bound |
| b. Very blurry | The cockpit view was 120 degrees tall, so 144 wide at 16:9, for eyes that see 108 x 99 together: 69 % of the picture's pixels were outside both eyes, 7.3 pixels per degree in the middle | The view covers exactly the eyes plus the 5 degree margin: 123 x 109 degrees around an axis 5.5 degrees down, with its own projection aspect: 12.0 x 9.0 pixels per degree in the middle at render scale 2, 62 % of the pixels inside the eyes (the rest is the margin) |
| b. Cockpit latency 70-95 ms against 40 on the screen | The 40 was never measured: the estimate starts at 40 ms and only cockpit frames (tagged with the time their camera was built) measure it. The 70-95 ms is the game's own pipeline plus WiVRn's; ours was the prediction, capped at 50 ms, 30 ms short | Predicted as measured up to 100 ms; the latency is split into the game's and the runtime's part in the 10 s line and the trace; `latency -` without cockpit frames |
| d. Looking back about 180 degrees "changes the position" | Turning round moves the head back (the eyes pivot round the neck, the shoulders turn). The driver's seat is about 20-25 cm behind the game's eye point (the test save's Koenigsegg): from about 15 cm back the near plane cuts through the seat and the view lands inside it, which looks like another camera. The camera itself stays `CCamDriver`, posed (trace) | The eye stops 8 cm behind where it was recentred (`vr_head_back_limit_cm`) |
| c. HUD outside the view and giant; menus giant on the screen | The screen was 3.2 m at 2.5 m: 65 degrees | 2.2 m: 47 degrees; the HUD in the cockpit: below |

### Camera physics off in the VR cockpit (h, i)

Each frame the director's step (`sub_82855070`, TU4 `82905418`, from the director update
`sub_82858288`, TU4 `82908710`) has the active camera build its own view (vtable +24: for
`CCamDriver` (TU4 `82901D58`) a look-at from the driver's eye on the car body, along the car's
forward turned by the look-around angle, with the car's up), its field of view (+28) and its
position (+40), then calls `sub_82853DD0` (TU4 `82904110`) with the camera, the view matrix, the
FOV and the position. That function applies what the camera's physics layers computed this frame
(camera + 48 ... + 112, written by the camera's update): yaw/pitch/roll in camera and car space,
car-space position offsets along the car's axes, a forward offset, the FOV offset, and rebuilds
the matrix (`sub_828FD278` in TU4, a look-at from the new position, forward and up).

The hook on `sub_82853DD0` returns at once while the head pose owns the camera: the VR cockpit
with an in-car camera (`CCamDriver`, `CCamCockpit`), unless `vr_camera_effects`; or the test
pose (`vr_head_pose_test` with a pose set). The view stays the camera's own, rigid with the car
body (which still pitches and rolls on its suspension, as a real seat does); the FOV is the
camera's own (62 degrees, which the VR cockpit replaces); the layers keep running, so switching
back is smooth. Other cameras keep their effects.

Checked with the emulated headset (`scratchpad/xremu`, Monado with a scripted head): the same
drive (reverse out of the outpost's parking, accelerate to about 80 km/h, into the fence), 12
game pictures 0.3 s apart, the dashboard band (rows 70-95 %, below the windscreen) matched
between consecutive pictures by shifting it: with `vr_camera_effects=true` (version 1's
behaviour) it moved by up to 36 x 12 pixels (about 3 x 1.3 degrees) from one picture to the next,
with the default by 0 in all 11 pairs, through the crash too.

### The cockpit's field of view (b)

`CoverEyes` (`xr_math.h`) takes both eyes' fields of view and orientations relative to the head
(`xrLocateViews` in `VIEW` space, so canted displays are handled) and returns the drawn frustum:
the axis at the middle of the eyes' extreme angles (Quest 3: 0 degrees across, (44 - 55) / 2 =
5.5 degrees down), and the half-angles that cover every corner of both eyes' frusta seen from
that axis, plus `vr_fov_margin`. Pitching the axis widens the corners a little (Quest 3: 56.7
instead of 54 degrees across), but it drops 11 degrees of height: on balance more pixels per
degree on both axes than a straight axis (13.4 x 7.3).

The game then draws with the head turned to that axis (the camera hook composes the turn into
the head transform), the vertical field of view in the director's +304 and the aspect from the
viewport query (`sub_823E22C8`, whose hook returns it while a posed view is handed over: the
presentation camera's `SetAspectRatio` and the game-side frustum both ask it inside the two
readers). The picture keeps its pixels (2560 x 1440 at render scale 2), so they are not square:
123 degrees across 2560 pixels, 109 down 1440. The projection layer gets the turned head pose
and the frustum's symmetric field of view; the runtime maps the picture onto it.

| | Version 1 | Version 2 |
| --- | --- | --- |
| Drawn (Quest 3 eyes L -54/40/44/-55, R -40/54/44/-55, margin 5) | 144 x 120 degrees, straight | 123 x 109 degrees, axis 5.5 down |
| Pixels per degree in the middle, render scale 2 | 7.3 x 7.3 | 12.0 x 9.0 |
| Pixels inside either eye | 31 % | 62 % (90 % without the margin) |

The headset itself has about 16 pixels per degree in the middle at the recommended 2064 x 2162
per eye: render scale 3 would come closer (18 x 13.5), at the GPU cost of 2.25 times the pixels.
The ultrawide machinery ([ultrawide.md](ultrawide.md)) can also draw at another raster aspect,
but only wider than 16:9, Vulkan only, and from the desktop window's size: the aspect query is
the part that fits here. In the emulated headset with the same eyes the layer reads `fov_deg l
-61.74 r 61.74 u 54.50 d -54.50`, pitched 5.5 degrees down (`quat_xyzw -0.048 0 0 0.999`), and
the eye views match the game's picture at every pose of the standard sequence (straight, yaw
45/90/180, pitch, roll, leans).

### Pads without window focus (a)

Patch 0392 adds `rex::input::SetPadsWithoutFocus` (C export `rexinput_SetPadsWithoutFocus`).
The OpenXR thread turns it on when the session begins (`Input: pads and wheels work while the
window has no focus`) and off when it stops or is destroyed (`... need the window's focus
again`). It also sets SDL's background joystick events hint (SDL ignores presses while no window
has keyboard focus; the wheel driver sets the same hint when `wheel_mode` is on, the default).
The keyboard and mouse driver checks the focus itself, so typing in another window does not
drive the car. Without the patch the game logs that the pad needs the focus (click the game
window before putting the headset on). Not checked with a real pad in a headset session: the
emulator runs drive through the test FIFO, which does not go through the SDK's drivers.

### Latency (b)

The `latency` of the 10 s line is the time from the camera hook building a posed view to the
display time of the headset frame that first shows it. Measured in the emulator at 30 fps: about
88 ms of game (the camera is built at the start of a main loop frame, the render thread draws it
during the next one, then the GPU and the swap; plus up to one headset frame until the OpenXR
thread takes the picture) and 10-11 ms of runtime (Monado, no network). At 72 fps the game's part
should be near 40 ms; WiVRn adds encoding, the network and decoding. So the 70-95 ms of the
first test is mostly not ours. What was ours: the pose was predicted at most 50 ms ahead, so the
picture was drawn for a head 20-45 ms older than when it was shown, and the runtime reprojected
the difference (rotation only: head translation in that time made the cockpit swim). The cap is
100 ms now. Next, if needed: re-pose the view on the render thread when it takes the matrix
(`sub_823EB7A0`), about one game frame later, which would also need the render-side view state
(`sub_82DBADC0`) re-posed.

### Looking back (d)

The emulated head turned 90, 135, 170, 180 and 190 degrees, over each shoulder (150 degrees and
15 cm aside, 10 cm back), and at 180 degrees moved 0, 5, 10, 15 and 20 cm back. The camera stayed
`CCamDriver`, posed, with the game's look-around angle at 0 (trace). At 180 degrees the view is
the driver's seat seen from the front, filling the view (there is no body); 15 cm back the near
plane (0.1 m) cut through the seat, 20 cm back the view was inside it (dark, the rear bulkhead):
another position, as reported. With the 8 cm limit the views at 15 and 20 cm back are the 8 cm
view. Sideways and upward moves are not limited (at 180 degrees, 15 cm aside or 10 cm up shows the
seat's side or top). The trace shows the head's position, so a real look back shows how far it
went.

### HUD and menus (c)

The screen is 2.2 m wide at 2.5 m: 47 x 27 degrees, the corners 24 degrees from the middle, within
an eye turn without turning the head, about what a cinema seat gives (THX recommends 36-40
degrees). The menus, drawn for 1280 x 720, get 27 of their pixels per degree, a little more than
the headset shows in the middle (about 16 at WiVRn's recommended size, 25 for the panel), so text
is not magnified into blur; the copy for the screen is halved from render scale 2 so that the
runtime's unfiltered sampling does not shimmer. Version 1's 65 degrees needed head turns to read
the corners.

In the cockpit the HUD is drawn into the picture at its usual places. With the new field of view
the map spans 48-59 degrees left and the speedometer 48-59 degrees right of the view's middle,
in the lower corners: each at the outer edge of one eye only (the left eye sees to 54 degrees left,
the right eye to 54 right), mostly in the margin and following the head. The narrower view brings
them nearer (version 1: 60-72 degrees, outside both eyes) but not into a readable place. The
game's HUD options can hide them; the car's own gauges are readable in the cockpit. A HUD quad
of its own needs the scene/HUD split per frame (not done).

### The trace (`vr_trace`)

`--vr_trace=true` (or `vr_trace = true` in `forza.toml`) logs from the OpenXR thread about 10
times a second:

```
VR trace: head yaw 180 pitch 0 roll 0 deg, x 0.0 y 0.0 z -15.0 cm; camera CCamDriver 820860FC
look 0 deg, posed; shows cockpit 123x109 deg pitch -5.5; latency 99 ms (game 88 + runtime 11)
```

(one line in the log; about 8 MB an hour). The head relative to the recentred pose at the frame's display time: yaw
left, pitch up, roll left in degrees, x right, y up, z forward in cm (the game's axes; z -15 is
15 cm back). The game's active camera (class from its vtable, or `?`; `no game view` when the
camera hook has not run for 0.5 s: menus, loading) with its look-around angle (the right stick)
and whether the head pose went into it. What the headset shows (cockpit with the frustum drawn,
or screen) and the last cockpit frame's latency in its two parts.

### Not done in version 2

- **Motion blur and depth of field** stay on. The render scenario files (`renderscenarios.zip`,
  e.g. `Horizon_Race.xml`) switch them (`MotionBlur`, `DepthOfField`), read through the game's
  reflection classes (setting ids 4 and 3, vtables at `0x8223C1CC` and `0x8223C1F8` in the disc
  executable); where the renderer reads the value was not found yet. Motion blur reprojects with
  the previous frame's camera, so quick head turns smear the world.
- Stereo, a HUD quad, late latching on the render thread, Windows with a headset.

### Tested (version 2, Linux, TU4, RX 9060 XT)

- **Emulated headset** (`scratchpad/xremu`: Monado 25.1 with a scripted head, the first test's
  eye fields of view, 1032 x 1081 pixels per eye, 90 Hz; its timing is not real): cockpit mode at
  render scale 2 with the trace on: the standard pose sequence (straight, yaw 45/90/180, pitch 30
  up and down, roll 20, leans 15 cm, forward 10, up 5), the look-back sequences above, the drive
  with and without camera effects, the chase and hood cameras on the screen (`quad_size 2.200
  1.237`, the picture halved to 1280 x 720 on the way). Clean session start and end.
- **VR off** on scratch data: no `VR` line, no OpenXR library mapped, the in-car view as before,
  clean exit.
- **Unit tests:** `xr_math_test` (the covering frustum for the Quest 3's eyes, canted and lopsided
  eyes, caps, the turned view in OpenXR and in the game, yaw/pitch/roll, 180 and -180 and q/-q,
  the back limit, the latency cap), `xr_frame_tags_test`, `vr_head_pose_math_test`, the launcher's
  `test_vr.py`.

## HUD and menus

**Where.** The game's UI system (`ForzaUI` / UI4: `CHud` and its controls, the menus'
screens) draws on the render thread after the scene's post-processing, straight into the
front buffer: screen-space quads with the HUD/font shader families, no depth test. In the
prototype's screenshots the minimap, speedometer, lap/position boards and the event
banners stay where they are on the screen whatever the head does.

**How the SDK already separates it.** Patch 0046 (`VulkanCommandProcessor::IssueDraw`)
finds each frame's scene/UI boundary from the front buffer (1280 pitch, no MSAA): the
tonemapped scene is composited into it as `k_2_10_10_10`, and the first draw into the same
buffer as `k_2_10_10_10_AS_10_10_10_10` starts the UI (`fh1_scene_composited_`,
`Fh1SelectNativeHud`). With DLSS/FSR 3 the UI already goes to its own render targets at its
own scale, seeded with the scene. Patch 0063 also recognizes the HUD/font shader families
and moves whole HUD draws (the ultrawide edge anchors).

**For VR.** Seed the UI target with transparent black instead of the scene and hand it to
the compositor as a quad layer: head-locked (or a fixed panel in front of the seat) for
menus, fixed in the car below the windscreen for the HUD. To check: elements that darken or
blend with what is under them (translucent backdrops) need the alpha to come out right on
a cleared target. Projected labels (rival names, route markers) were not in
the screenshots' frames; whether the scene pass or the HUD pass draws them is open (if the
HUD pass, they need a per-eye position or a depth in stereo). Menus over a
paused scene (pause, map) and the 3D menus with their own UI cameras (garage, showroom,
`CUI4CarTargetCamera`) work unchanged on a flat virtual screen (stage a); the cockpit
gauges are part of the car model and are already in the car.

## Stereo: drawing the scene twice

**The engine has a second view.** The world update runs a fixed loop over two
`CSplitView`s, each with its own director, presentation camera and viewport, and the
executable and media still carry Forza Motorsport 4's two-player split-screen:
`ForzaUI::CSplitscreenLobby`, `CHudSplitscreen`, `CPauseSplitscreen`, a Kinect split-screen
join flow, `Splitscreen.str` (player 2 can "Play as Guest", no second profile needed), the
split-screen render scenario below, and split-screen cockpit offsets in the car data
(`SplitscreenCamCockpitOffsetX/Y/Z`, `Driver\SplitscreenDefaultOffsetFromSteeringWheel*`).
FH1 was not sold with split-screen; whether its menus still reach it is not established. So "the game draws the scene twice"
does not need a new engine path: the second split view is one. What it would take for
stereo: give view 1 the same player's camera with the other eye's offset (the prototype's
hook point, t.x = +-IPD/2), and its own full-size eye target instead of half of the
screen. Unknown: whether a split view can exist without a second player and car, and
whether the split-screen renderer can be pointed at two full-size targets.

**What the console cut to afford it.** `media/renderscenarios.zip` > `Splitscreen_Race.xml`
against `8-Car_Race.xml` / `Horizon_Race.xml`: tiling scenario 4
(`SplitScreen_NonTiling2xMSAA`: 2x MSAA in one pass instead of 4x MSAA in three predicated
tiles), no crowd (`CrowdLoad 0`), a quarter of the particles, no dynamic environment-map
updates (`EnvMapFrequencyScale 0`), shadow maps only in the cockpit view
(`SkipShadowMapUnlessCockpit`), a cheaper car shader (`shaders_splitscreen`), and no motion
blur entry. Those are the per-view costs; shadows and the reflection cube would be shared
between eyes anyway.

**What runs per scene draw, and what a second one costs** (estimates from the per-thread
measurements in [perf.md](perf.md) > "CPU time per frame" and "Low-end CPUs": TU4, 30 fps,
Ryzen 7 7800X3D at 5 GHz; not measured with two views):

| Per frame at 30 fps | Free roam | Race | Doubles with a second eye? |
| --- | --- | --- | --- |
| GPU command processor (PM4 parsing, register writes, `UpdateBindings`, texture and render target lookups, Vulkan recording) | 5.0 ms | 6.7-8.4 ms | its scene share, yes |
| Render thread (`sub_8261E178`): culling, per-object constants (`matWVP` ...), D3D command building | ~2.3 ms own work (3.5-4.1 ms with its waits) | ~2.3 ms (4.1-4.7) | its scene share, yes |
| Vulkan submission thread (driver) | 1.2 ms | 1.5 ms | its scene share, yes |
| Main loop (simulation, physics, AI) | 2.3 ms | 2.7 ms | no |
| Audio mixer and decoders | 3.5 ms | 4.4 ms | no |
| GPU (RX 9060 XT, 720p) | 7.9 ms | 8.3 ms | scene and post, yes; shadows, cube map, HUD, no |

**The scene's share, measured.** A RenderDoc capture of free roam (TU4, RADV, 720p, hood
camera, standing at the spawn; `renderdoccmd` with `FORZA_RDCAPTURE_TRIGGER`, draws grouped by
their render targets) holds about 3,000 host draws per guest frame:

| Pass | Draws per frame | Targets |
| --- | --- | --- |
| Main scene, 4x MSAA in three EDRAM tiles | 1,969 (about 660 per tile) | 1280x512 4x MSAA colour and depth |
| Shadow cascades, depth only | 320-335 | 1040x2528 and 520x1264 depth |
| Another depth-only pass (not identified) | 388 | 1280x2048 depth |
| Post-processing, bloom, quarter-resolution passes | about 250 | 1280x2048 and 640x1024 colour |
| Reflection cube update (not every frame) | about 15 | 320x4096 and 160x4096 |
| Front buffer, HUD and the rest | the remainder | |

So the scene is about two thirds of the draws the command processor turns into Vulkan, and
a second eye in the same tiled mode adds about 66% to that thread: 5-8.4 ms becomes about
8.3-14 ms per frame. With the render thread's and the submission thread's scene shares, a
second eye adds roughly +5 to +8 ms of CPU per frame here, on the two threads every frame
waits for. That still holds 30 fps on this CPU, halves the ceiling above 60, and puts 30 fps
out of reach on a Steam Deck-class CPU (command processor 2-2.5 times slower). The
integration research below reaches the same verdict; it estimates +10-13 ms by doubling both
threads whole.

**A lead that changes the sums.** The game builds the scene's command buffer once and the
GPU replays it per tile (predicated tiling), so the emulated GPU processes the scene three
times while the host render targets need it once. The split-screen scenario's non-tiled
mode (`TilingScenario 4`, `SplitScreen_NonTiling2xMSAA`) draws the scene in one pass. If a
non-tiled scene (or skipping the redundant tiles in the SDK) works for the main view, it
would cut the command processor's scene work to about a third, which is more than a second
eye costs, and would help slow CPUs in flat play too (#45). Not tried. A two-player
split-screen race measured against a one-player race would replace the estimates above with
numbers (open question).

**Alternatives** (details in the research section): rendering one wider mono view and
warping it to both eyes with the depth the DLSS/FSR 3 path already decodes (CPU flat,
stereo approximate), or producing both eyes inside the GPU emulation (multiview, or replaying
the frame's command stream per eye; CPU about flat, GPU about 2x, a large SDK job). For the
latter: the per-object constants are products the render thread computed (`matWVP`), so a
host-side per-eye change has to be applied in clip space (`P_eye * T_eye * P^-1`, or the
`x += -P00 * dx` shift), not by swapping a view constant. Drawing alternate eyes on
alternate frames is out: at 30 fps each eye would update at 15 Hz while the car moves
almost a metre per frame.

## Integration and stereo options (research)

From a separate research run (OpenXR, runtimes, how other projects put flat games in VR); nothing in it was built. Checked against the camera and render-loop findings above; corrections are marked **Checked:**, and its open questions are answered [below](#answers-to-the-research-questions).

Status: this is research only. Nothing has been built or run. CPU, GPU and pixel figures are estimates taken from [perf.md](perf.md), [framerate.md](framerate.md) and simple geometry. The few measured numbers are marked as measured. [Open questions](#open-questions-for-the-camera-and-render-loop-work) lists what the camera and render-loop work had to answer; the answers follow this section.

### Summary

- **The game must never draw the scene twice.** In a 30 fps race on a 5 GHz Zen 4 core, each frame costs about 4.7 ms of render-thread work and 6.7-7.1 ms of the emulated GPU's command processor (measured, [perf.md](perf.md)). **Checked:** about 2.3 ms of the render thread's 4.7 ms is the game's own work; the rest is waiting for the GPU ([perf.md](perf.md) > Low-end CPUs), so a second eye adds the scene's share of about 2.3 + 7 ms, not 12 ms (estimate in [Stereo](#stereo-drawing-the-scene-twice)).
  - Every frame waits on the command processor, and it is the limit above 60 fps ([framerate.md](framerate.md)).
  - A second guest scene submission doubles both threads. That cuts a fast desktop's race ceiling roughly in half and puts 20-22 fps machines far below 30.
- **The game keeps its own frame rate, and the OpenXR runtime drives the headset.**
  - A separate XR thread resubmits the newest finished game frame at 72-120 Hz. Each frame is tagged with the head pose it was rendered with.
  - The runtime reprojects head rotation. The game is never blocked by `xrWaitFrame`.
- **Build order:**
  1. OpenXR plumbing and a virtual screen.
  2. A head-tracked mono cockpit, with the HUD on a quad layer.
  3. Stereo by depth reprojection of that mono view.
  4. True stereo inside the GPU emulation: Vulkan multiview or per-eye replay. Stage 3 stays as the fallback for weaker GPUs.

  Stages 1-3 keep the CPU cost at flat. Stage 4 keeps it close to flat, but it is a large GPU-emulation job.
- **Not built:** alternate-eye rendering, a wide render cropped per eye, and the game drawing twice.
- **Vulkan only at first.** Every PC runtime that #333 targets supports `XR_KHR_vulkan_enable2`. D3D12 is a later port.
- **PCs that run the game at about 20-22 fps get the virtual screen.** No precedent treats head-tracked 3D at 20 fps as comfortable.

### What this game imposes

- **CPU.** See above. Anything that makes the guest submit the scene twice doubles the two threads that limit the frame.
- **Frame rate.**
  - The default is 30 fps; 20, 40 and 60+ are available.
  - The frame rate is the guest vblank divided by a divider. Dividers 2 and 3 exist today (`frame_pacing::VblanksPerFrame`). 20 and 40 fps keep real-time speed (#263).
  - In VR the guest vblank should become the headset refresh rate, which needs new dividers and rates. See [Pacing](#pacing-against-the-headset).
- **Output path.**
  - `VulkanCommandProcessor::IssueSwap` calls `Presenter::RefreshGuestOutput`, which writes into a 3-image mailbox. The images are A2B10G10R10 and stay in `SHADER_READ_ONLY_OPTIMAL`.
  - The mailbox is built for several consumers, and the presenter is one of them.
- **Existing scene/HUD splits.** There are two, and neither gives a clean HUD-only image with real alpha:
  - The HDR path (`CaptureHdrScene`) takes a snapshot of the scene before the first HUD draw. It finds HUD coverage by comparing the final picture with that snapshot, and the result goes into the guest output's 2-bit alpha.
    - It runs only while HDR is active.
    - It triggers only on the 1280-pitch transition from `2_10_10_10` to `2_10_10_10_AS_10_10_10_10`.
    - It is "a visual difference mask, not game-provided HUD geometry" ([hdr.md](hdr.md)).
  - The native DLSS path draws the HUD, menus and labels into their own raster at their own resolution. That raster is seeded with the upscaled scene ([native_dlss.md](native_dlss.md)).
- **Depth and motion vectors already exist.** The DLSS/FSR 3 work (SDK patches 0046/0057/0058/0072, `fh1_dlss.cpp`) provides:
  - The packed float24 depth decode (view z = B/(1-A-depth) from the `PPMotionBlurDepthHorizonVel` constants).
  - Camera and per-object motion vectors. The player's car uses stencil 15.
  - A main-scene draw filter: `z_enable`, `zfunc != Always`, not window-space, `surface_pitch == 1280`. The pitch test has to be generalized for VR sizes.
  - A per-draw `ndc_offset`.
- **Field of view.**
  - `hooks/ultrawide_projection.cpp` already widens the projection and the culling together.
  - Non-16:9 drawing works with a padded integer EDRAM stride, but only for wider images. A taller-than-16:9 image, such as a near-square eye, is untested ([ultrawide.md](ultrawide.md)).
- **ABI.**
  - The game is compiled against the unpatched nightly SDK headers. So any VR API that crosses the SDK boundary has to be `extern "C"` functions that the game finds with `forza::SdkSymbol`, like `ForzaWheelPlayerDriving` and `rexui_SetDialogContinuousRepaint`.
  - Patches must not change the layout of `VulkanDevice` or `Presenter`, because the game inlines their accessors.
- **Vulkan queue.**
  - The device has one graphics+compute queue. The presenter, DLSS and the "Vulkan Submission" thread from patch 0038 all share it through `AcquireQueue(...)`'s `recursive_mutex`.

### Recommended staged approach

#### Stage 1: OpenXR session and a virtual screen

- **What:**
  - SDK patch A, so the runtime chooses the GPU and adds its Vulkan extensions.
  - The XR thread.
  - A `LOCAL`-space quad, about 3.2 x 1.8 m at 2.5 m, showing the guest output.
  - The desktop window as a mirror.
  - Packaging: the loader, Flatpak permissions and a launcher toggle.
- **Needs from the reverse engineering:** nothing.
- **Cost:** CPU 1x. GPU about 1x at flat resolution. It works on every PC, including the 20-22 fps ones.
- **Value:** on its own this stage is limited, because Virtual Desktop and SteamVR's theater mode already give players a virtual screen. It is the base for everything later, and it stays the mode for menus, loading, movies, pause, photo mode and slow CPUs.
- **Done when:**
  - The session starts and stops cleanly on SteamVR (Windows and Linux), Meta Link, VDXR and Monado/WiVRn.
  - Any failure falls back to flat.
  - Flat play with VR off shows no change in frame time.

#### Stage 2: Head-tracked mono cockpit

- **What:**
  - The camera hook reads the head pose from the XR thread's pose slot: rotation plus seated translation.
  - Projection and culling are widened to the eye FOV plus a margin of about 10° for reprojection edges, as RPCS3 VR does. The drawing size is near-square.
  - SDK patch B exports the pre-HUD scene and the HUD every frame.
  - SDK patch C phase-locks the guest vblank to the headset.
  - Frame tags link each GPU frame to its pose.
  - A projection layer shows the same image in both views from the centre pose, and the HUD goes on a quad.
  - Comfort switches: shake, G-force head motion, motion blur and depth of field off.
- **What the player sees:**
  - No stereo. Rotation-only reprojection shows the image as if at infinity, so the cockpit looks oversized and flat.
  - This is a milestone more than a product. It may still be worth offering as an option on 30 fps machines.
  - In a seated cockpit, `LOCAL` space acts as car-fixed. The interior reprojects almost perfectly at headset rate, and the outside world steps at the game's rate.
- **Cheap experiment in this stage:** submit the decoded depth with `XR_KHR_composition_layer_depth` and the centre pose. Then check whether Meta's ASW 2.0 positional timewarp gives usable parallax "for free". This is unverified, and Meta only at best.
- **Needs from the reverse engineering:** the camera hook point, a widenable frustum that drives culling, near-square or off-axis projection, and frame-skip tracking. **Checked:** the hook point and the culling are done in the prototype (both readers of the view matrix are posed; render culling follows); widening goes through the same FOV/aspect the ultrawide hook already scales; off-axis projection and frame tags are still open.
- **Cost:** CPU 1x. GPU about 1.1-1.2x a mono view at headset resolution, because of the margin.

#### Stage 3: Stereo by depth reprojection

- **What:** render the centre view, covering the union of both eye frusta. Warp it to each eye by ±IPD/2 using the decoded depth, and fill disocclusions from the farther side.
  - Symmetric warping makes holes half the size of warping one eye into the other.
  - A temporal cache can help on the rigid cockpit.
- **Per-eye shift from the centre view** (32 mm half-IPD, about 15-25 px per degree on current headsets):

  | Distance | What is there | Shift per eye |
  |---|---|---|
  | 0.5 m | Steering wheel | 3.7°, 55-90 px |
  | 0.8 m | Dash, A-pillar | 2.3°, 35-57 px |
  | 1.2 m | Mirror | 1.5°, 23-38 px |
  | 5 m | Car ahead | 0.37°, 5-9 px |
  | 20 m | Scenery | 1-2 px |
  | 50 m and beyond | Far scenery | under 1 px |

- **What the player sees:** the world is close to correct. Each eye gets a 30-55 px filled band beside the wheel and A-pillars, which is exactly where the driver looks.
- **Risks:**
  - Glass that writes depth would flatten the world onto the windshield.
  - Motion blur, depth of field and MSAA edges misalign colour and depth. Turn blur off in the final combine but keep the velocity pass, because it produces the motion vectors.
  - Particles, lens flares and transparent surfaces take the wrong depth.
- **Later:** run the warp on the XR thread every headset frame with the newest pose. That is our own positional timewarp, and it works on every runtime, not only Meta's.
- **Precedents:** CryEngine 3's screen-space stereo (Crysis 2), the vorpX Z-buffer mode, and HindsightVR's Xenia fallback.
- **Cost:** CPU 1x. GPU about 1.15-1.3x (a wider centre view plus a warp pass under 1 ms).

#### Stage 4: True stereo inside the GPU emulation

The guest submits each frame once, and the Vulkan backend produces both eyes. Precedents: Dolphin, HindsightVR on xemu and Xenia, the RPCS3 VR fork, openRBRVR 2.0 and 3D Vision Automatic. openRBRVR's notes say multiview "reduces CPU load significantly", and RPCS3 VR moved to it as well.

**Variants:**
- **Multiview.** One draw writes two layers, and `gl_ViewIndex` picks the eye. Every main-scene render target, every resolve, and every texture sampled from a resolve becomes a 2-layer array.
- **Per-eye replay.** Replay the frame's recorded deferred command stream once per eye with a per-eye push constant.
  - Estimated cost: an extra 1.2-1.5 ms on the Vulkan submission thread.
  - It only works cleanly if the main scene and its post-processing fit in one submission. A mid-frame readback or memexport wait breaks it.

**Eye offset**, added as an epilogue to the translated vertex shader. HindsightVR's approach of the same SPIR-V for both eyes plus a push constant leaves the shader-cache key unchanged.
- **Clip-space shift.** `pos.x += -P00*dx` before the divide by w, plus the per-eye asymmetric frustum as an NDC scale and offset. `ndc_offset` is already applied per draw. This needs no knowledge of the game's matrices. Dolphin and 3D Vision use the related form `x += sep*(w - convergence)`.
- **Full reprojection.** `M_eye = P_eye * T_eye * P^-1`. This needs the game's projection matrix P. Keep the game's NDC z unchanged.

**Rules from the precedents:**
- Shadow, light-space and reflection passes stay mono.
- HUD draws and flat full-screen passes get no offset.
- Full-screen passes that rebuild position from depth need a per-eye inverse projection.
- Scissor rectangles are per eye.
- Readbacks and memexport happen once.
- HindsightVR's Xenia layered resolve was disabled at render scale above 1, so scaled resolves need care.

**Cost:** CPU about 1x. GPU about 1.7-2x, because shadows and cube maps are drawn once.

**Later optimization:** hybrid near-field stereo, as in Oculus's hybrid mono rendering. True stereo for the player car (stencil 15), reprojection beyond about 10 m. Its complexity is close to full stereo.

Stage 3 stays as the fallback for weaker GPUs.

#### Not building

- **Alternate-eye rendering** (UEVR AFR, Luke Ross AER):
  - At 30, 40 or 45 fps, each eye updates at 15, 20 or 22.5 Hz.
  - At 100 km/h the car moves 0.93 m per 30 fps frame. A roadside object 3 m to the side and 10 m ahead changes bearing by 1.6° between the two eyes' images, against a true binocular disparity of about 0.35°. The result is false depth or a double image, and rotation reprojection does not fix translation.
  - UEVR's documentation says its AFR mode "causes eye desyncs and usually nausea". Luke Ross says ghosting "becomes worse as the game frame rate gets lower".
- **Wide render cropped per eye:** every object sits on one depth plane. It costs about as much as stage 3 and is no better than mono.
- **Game draws the scene twice:** it doubles the render thread and the command processor, about +10-13 ms per frame at 5 GHz. It is only useful as a correctness reference on fast CPUs, and only if the engine has a clean extra-view entry point. **Checked:** it has one, the second split-screen view (left in from Forza Motorsport 4's split-screen); the scene's share of the two threads is about +5-8 ms per frame here. The verdict for slow CPUs stands.

#### Which PCs get what

| CPU class (flat fps) | Recommended VR mode | Game rate on the headset |
|---|---|---|
| Steam Deck class, 4-core laptops (20-22) | Virtual screen (stage 1) | Any; the quad is redrawn at headset rate |
| Holds 30 solid | Stages 2-3 | 30 on 90 Hz (1/3) or 120 Hz (1/4); 24 on 72 Hz is marginal |
| Holds 45-60+ | Stages 2-4 | Half the headset rate: 36/40/45/60 on 72/80/90/120 Hz |

- Frame synthesis (SteamVR Motion Smoothing, Meta ASW, Virtual Desktop SSW) is designed around exactly half rate. Motion Smoothing scales down to 2-3 synthetic frames per real one.
- How Meta ASW behaves below half rate is not confirmed here.
- **GPU check (rough, from a measurement taken while other games also loaded the GPU):**
  - On the RX 9060 XT, 1440p at render scale 2 (3.7 Mpx) ran 42 fps against a 60 target.
  - Two 1440x1440 eyes are 4.1 Mpx, so stage 4 would land around 36-40 fps on that card.
  - Stage 3 at about 2.3 Mpx leaves headroom.
  - A 1440-wide eye uses a 2560 EDRAM stride: wasted storage, not wasted shading.
  - This needs a real benchmark.

### Comparison of stereo methods

CPU is relative to flat at the same game frame rate. GPU is relative to one mono view at headset resolution; flat 720p is not a useful baseline. All numbers are estimates.

| Method | CPU cost | GPU cost | Comfort | Quality | Complexity | Verdict |
|---|---|---|---|---|---|---|
| Virtual screen (quad layer) | 1x | about 1x at flat resolution | Best: the room is a stable reference | 2D, not immersive | Low | Stage 1; menus, slow CPUs |
| Head-tracked mono | 1x | about 1.1-1.2x (margin) | No stereo; the cockpit looks oversized | Flat image, correct culling if widened | Low-medium (camera hook) | Stage 2 |
| Mono plus depth layer, runtime positional timewarp | 1x | about 1x | Unknown | Unknown; Meta only, untested | Low once depth is exported | Experiment in stage 2 |
| Depth reprojection to two eyes | 1x | about 1.15-1.3x | Good for the world; edge artifacts can strain the eyes | 30-55 px filled bands at the wheel and pillars; transparency wrong | Low-medium (inputs exist) | Stage 3, then the fallback |
| Multiview in the GPU emulation | about 1x (plus layered target and resolve bookkeeping) | about 1.7-2x | Correct stereo | Near native; some screen-space effects from the centre eye | Very high | Stage 4 |
| Per-eye replay of the recorded command stream | about 1x, plus 1.2-1.5 ms on the submission thread | about 1.7-2x | Correct stereo | Near native | Very high; needs one submission per frame | Stage 4 alternative |
| Host re-translates every draw twice | Guest 1x; command processor 1.3-2x | about 2x | Correct stereo | Near native | High | Avoid: doubles the high-fps limit |
| Wide render cropped per eye | 1x | about 1.1-1.2x | No depth; one plane | Looks like a cardboard photo | Low | Do not build |
| Alternate-eye rendering | 1x | 1x (plus optical flow if AER v2-like) | Poor in a moving car | Ghosting, false depth | Medium | Do not build |
| Game draws the scene twice | About 2x the scene's share of the render thread and command processor | about 2x | Correct, but at a much lower frame rate | Best | Medium-high: the second split-screen view exists | Reference only (fast CPUs) |

In every method the runtime corrects head rotation only. Scenery outside the windows still steps at the game's frame rate. The car-fixed cockpit acts as a rest frame, which helps against vection.

### OpenXR integration sketch

OpenXR is a second consumer of each finished game frame, running on its own thread, not a new `Presenter`. A game-side "Forza XR" thread:
- owns the `XrInstance` and `XrSession`;
- runs `xrWaitFrame`, `xrBeginFrame` and `xrEndFrame`;
- owns the per-eye and quad swapchains.

#### Who owns what

- **Game repo** (logic stays out of the patch stack):
  - `forza-recomp/src/platform/xr_session.{h,cpp}`: instance, system, session, spaces, events.
  - `xr_frame_loop.cpp`: the XR thread.
  - `xr_pose.h`: the pose seqlock and frame tags.
  - `xr_math.h`: pose and FOV maths, unit-tested like `tests/frame_pacing_test.cpp`.
  - `thirdparty/OpenXR-SDK` for the loader (Apache-2.0).
- **SDK patches A, B and C** below, all exposed as `extern "C"` functions.

#### Startup and shutdown

1. **Early setup with `--xr`**, in `ForzaApp::OnPostInitLogging` or at the top of `OnPreSetup` before `LoadGpuPlugin`:
   - Call `xrCreateInstance` with `XR_KHR_vulkan_enable2` and `XR_KHR_composition_layer_depth`. Add `XR_KHR_convert_timespec_time` on Linux or `XR_KHR_win32_convert_performance_counter_time` on Windows.
   - Call `xrGetSystem(HMD)`, then `xrGetVulkanGraphicsRequirements2KHR`. The spec requires that call before `xrCreateSession`.
   - Register patch A's hooks.
   - If any step fails, log it and run flat.
2. **The SDK creates the instance, physical device and device through the runtime** (patch A).
3. **In `ForzaApp::OnCreateDialogs`**, where `InstallVulkanHooks` already runs and the provider exists:
   - Create the session with `XrGraphicsBindingVulkan2KHR{instance, physical_device, device, queue_family_graphics_compute(), queue_index}`.
   - Create the `LOCAL` and `VIEW` spaces and the swapchains.
   - Start the XR thread.
4. **On close**, in `OnWindowCloseRequested`/`OnShutdown`: call `xrRequestExitSession`, then destroy everything. The SDK hard-exits right after close, so this has to happen there.

#### SDK patch A: Vulkan creation through the runtime (`librexruntime`)

The runtime has to pick the physical device and add its own instance and device extensions. That happens inside `VulkanInstance::Create`, `VulkanProvider::Create` and `VulkanDevice::CreateIfSupported`. The existing `vulkan_hooks.cpp` only swaps function pointers after the device exists, which is too late.

```c
struct RexVulkanCreationHooks {
  void* user;
  VkResult (*create_instance)(void*, PFN_vkGetInstanceProcAddr, const VkInstanceCreateInfo*, VkInstance*);
  VkPhysicalDevice (*physical_device)(void*, VkInstance);          // xrGetVulkanGraphicsDevice2KHR
  VkResult (*create_device)(void*, PFN_vkGetInstanceProcAddr, VkPhysicalDevice,
                            const VkDeviceCreateInfo*, VkDevice*);  // xrCreateVulkanDeviceKHR
};
extern "C" void rexui_SetVulkanCreationHooks(const RexVulkanCreationHooks*);
```

- **Instance:** both `vkCreateInstance` call sites (the first attempt and the retry without layers) go through `xrCreateVulkanInstanceKHR`.
- **Physical device:** when the hook returns a device, try only that one, and log if the `vulkan_device` or `gpu_vendor` settings disagreed. If `CreateIfSupported` rejects it for missing emulation features, VR fails and the game runs flat.
- **Device:** `vkCreateDevice` goes through `xrCreateVulkanDeviceKHR`.
  - Extensions the runtime adds do not appear in `extensions_`, which is harmless.
  - Monado wants the external memory, semaphore and fence extensions.
- **Optional:** request a second graphics queue when `queueCount >= 2` and give index 1 to XR. NVIDIA exposes several; RADV exposes one.
  - Otherwise share queue 0 and take `AcquireQueue(family, 0)` around `xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage` and `xrReleaseSwapchainImage`. The spec lists exactly these as needing external synchronization of the queue.
  - `queue_families_[i].queues` is already a vector, so a second queue does not change the layout.

#### SDK patch B: per-swap eye and HUD export (`librexgpu-xenos`)

- **Changes in `IssueSwap` when VR is on:**
  - Run `CaptureHdrScene` and the HUD-coverage pass whether or not HDR is on, and generalize the `hdr_scenes_` ring.
  - In the same deferred stream, copy the pre-HUD scene and the final output with HUD coverage into a ring of 3-4 VR slots owned by the plugin. Being in the same stream keeps queue order, so no cross-thread GPU sync is needed.
  - Tag each slot with `swap_index`, the count of `IssueSwap` calls, counted before the async-placeholder skip.

  ```c
  struct RexVrFrame { uint64_t swap_index; VkImage scene, output; VkImageView scene_view, output_view;
                      uint32_t width, height; uint32_t slot; };
  extern "C" void rexgpu_VrEnable(int on);
  extern "C" int  rexgpu_VrAcquireLatest(RexVrFrame* out); // newest slot whose submission fence completed
  extern "C" void rexgpu_VrRelease(uint32_t slot);         // XR copy finished (its own fence signalled)
  ```

- **The XR thread only takes completed frames.** It never stalls, never shows a half-written image, and does not depend on how patch 0038 orders the refresher's work against the tracking fence.
- **Stage 4 extends this export** with a second layer per slot.

#### SDK patch C: vblank phase lock (`graphics_system.cpp`, the "GPU VSync" worker)

```c
extern "C" void rexgpu_SetVblankPhase(uint64_t guest_tick);
```

- **What it does:** the XR thread converts `predictedDisplayTime` to the guest tick with the timespec/QPC conversion extension. The worker then nudges `last_frame_time` like a PLL, so guest vblanks lock to the display.
- **Why it is needed:** without it, the SDK's 1 ms timer beats against the headset clock. Frames then alternate between 2 and 4 display periods, the same problem `PresentPacer` fixes for monitors (#263).

#### Frame tags: the pose a frame was rendered with

- **Writing the pose.** After `xrWaitFrame`, the XR thread writes head and eye poses in `LOCAL`, plus each eye's `XrFovf`, into a seqlock.
  - It uses `xrLocateViews` at `predictedDisplayTime + latency`.
  - The latency is a measured EMA from game-frame start to display, capped at about 30-50 ms.
- **The camera hook** reads that slot when it builds the view. It stores `FrameTag{eye_pose[2], fov[2], eye_mask, target_time}` for the main frame it belongs to.
  - The render pass (`sub_8259F3E8`, counter at render object `+0x948`) can skip main frames, so the tag must follow the frame the render thread actually draws.
- **At VdSwap** (next to `frame_time::OnGuestSwap`), the game pushes the tag with its swap count. VdSwap and `IssueSwap` are 1:1, so `RexVrFrame::swap_index` pairs each GPU frame with its tag.
- **Pose rule from the spec:** each projection view carries the pose and FOV the image was rendered with, not the newest pose. The runtime maps the difference, which is what makes its reprojection correct.

#### Swapchains and format conversion

- **Eyes:** one swapchain per eye, not a 2-layer array, so a single eye can be updated and the other resubmitted.
  - Size: the game's scene raster, not the recommended size; the runtime scales.
  - Format: `R8G8B8A8_SRGB`/`B8G8R8A8_SRGB` from `xrEnumerateSwapchainFormats`, created with `MUTABLE_FORMAT | COLOR_ATTACHMENT (| STORAGE)`.
- **Conversion:** one small pass samples the A2B10G10R10 source and writes the already gamma-encoded values through a **UNORM view** of the sRGB image. A `vkCmdBlitImage` into sRGB would encode twice.
  - The image is then moved to `COLOR_ATTACHMENT_OPTIMAL` before `xrReleaseSwapchainImage`, as the spec requires.
- **Resubmitting old frames is fine.** `xrEndFrame` uses the most recently released image of each swapchain. A game frame shown for 3 headset frames needs no new acquire or copy.

#### The XR thread loop

```
loop: xrPollEvent -> READY: xrBeginSession(STEREO) / STOPPING: xrEndSession / LOSS_PENDING: flat
  xrWaitFrame(&fs)                                   // only this thread blocks on the headset
  write pose slot (xrLocateViews(LOCAL, fs.predictedDisplayTime + latency))
  [queue lock] xrBeginFrame
  f = pacer.Pick(rexgpu_VrAcquireLatest, fs.predictedDisplayTime)
  if f is new && fs.shouldRender: acquire eye/HUD images, record convert pass, submit;
                                  [queue lock] xrReleaseSwapchainImage; rexgpu_VrRelease(previous)
  layers = driving ? { Projection(LOCAL, views[i] = {tag.eye_pose[i], tag.fov[i], eye[i]}, depth optional),
                       Quad HUD }
                   : { Quad cinema }
  [queue lock] xrEndFrame(fs.predictedDisplayTime, layers)
```

- The pacer picks a frame by its display slot, not simply the newest complete one. It holds frame N+1 until its slot, reusing `PresentPacer`'s adaptive-percentile offset from `frame_pacing.h`.

#### Pacing against the headset

- **The game never blocks on `xrWaitFrame`.** Instead, the guest vblank becomes the headset rate, phase-locked by patch C, and the divider is headset Hz / game fps.

  | Headset | Half rate (fast CPUs) | Lower rates |
  |---|---|---|
  | 72 Hz | 36 | 24 |
  | 80 Hz | 40 | 20 |
  | 90 Hz | 45 | 30 |
  | 120 Hz | 60 | 40, 30 |

- **New work:** dividers 2 and 3 exist. A divider of 4 and the 24, 36 and 45 fps rates are new, and real-time simulation speed at those rates needs checking.
- **The desktop window becomes a mirror:**
  - `QueuePresent`'s `PresentDelayNs` returns 0.
  - The mirror uses MAILBOX or IMMEDIATE. In `kGuestOutputThreadImmediately` mode the command-processor thread presents, so a 60 Hz FIFO monitor would throttle emulation.

#### Depth and frame synthesis

- **Depth layer:** `XR_KHR_composition_layer_depth` is supported by every runtime in the Khronos inventory.
  - The source is the depth that the DLSS/FSR 3 path already reconstructs. It needs near/far values and reversed-Z handling.
  - Meta's ASW 2.0 uses depth for positional timewarp.
- **App motion vectors** (`XR_EXT_frame_synthesis`, `XR_FB_space_warp`):
  - The inventory shows support only for SteamVR on Steam Frame among PC runtimes. It is not on Link or SteamVR for other headsets.
  - Its ratification status is unclear. Re-check before relying on it.

#### Packaging

**Checked** (and partly done): [Packaging](#packaging) under the OpenXR session.

- **Loader:** ship `openxr_loader` (`.dll`, `libopenxr_loader.so.1`).
- **Flatpak:** add the following, which match WiVRn's instructions for Flatpak apps:
  - `--filesystem=xdg-run/monado_comp_ipc`
  - `--filesystem=xdg-run/wivrn:ro`
  - `--filesystem=xdg-config/openxr:ro`
  - `--filesystem=xdg-config/openvr:ro`
  - A system-wide `/etc/xdg/openxr` runtime is not visible inside the sandbox.
  - Whether host-installed runtime libraries load inside the sandbox is open.
- **Launcher:** the VR toggle sets `--xr`, forces Vulkan, and picks a frame rate for the headset's refresh rate.

#### D3D12 (later, Windows)

- **Adapter:** pick the one whose `AdapterLuid` matches `XrGraphicsRequirementsD3D12KHR::adapterLuid`, and create the device at `max(11_0, minFeatureLevel)`.
- **Binding:** `XrGraphicsBindingD3D12KHR{device, direct queue}`. Images go back to `RENDER_TARGET` state at release.
- **Main cost:** re-implementing patch B. The D3D12 plugin has no pre-HUD snapshot or scene/HUD split.
- **Coverage without it:** WMR is the only listed PC runtime without Vulkan support, and Windows 11 24H2 removed it. VDXR reaches Vulkan through its interop layer.

### HUD, menus and overlays

- **When not driving** (`forza::telemetry::IsDriving()` is false): menus, loading, movies, pause, photo mode and the map go on the cinema quad. Every serious precedent does this, including RPCS3 VR, HindsightVR, ETS2 and openRBRVR.
- **While driving:**
  - The eyes get the pre-HUD scene.
  - The HUD goes on a `LOCAL` quad about 1.2 m ahead, below the eye line, using `BLEND_TEXTURE_SOURCE_ALPHA | UNPREMULTIPLIED_ALPHA`, drawn after the projection layer.
  - Distance and size should be user-adjustable.
  - A HUD baked into the eye image would follow the head, sit in the periphery of a wide eye FOV, and conflict with cockpit depth in stereo.
- **Use quads, not cylinders.** SteamVR has no cylinder layers.
- **Extracting the HUD is the open problem.** Neither existing split gives HUD-only RGBA. Options:
  1. **Difference mask between the final picture and the snapshot** (version 1):
     - Write it to a separate dilated 8-bit image; the guest output's 2-bit alpha is too coarse.
     - Opaque elements work.
     - Translucent panels carry a frozen copy of the scene behind them, at the wrong depth and updating at the game rate.
  2. **Redirect HUD draws** (everything after the scene-to-HUD transition) into a cleared transparent target. This is clean if the game's HUD blending writes usable alpha, which is unknown.
- **HUD layout:** the HUD is laid out for the eye raster's aspect ratio. Patch 0063's HUD edge anchors can pull elements inward.
- **World-space markers** (GPS arrows, checkpoints, name tags): if the 3D pass draws them, they stay correct in the scene. If they are screen-space overlays, they land on the HUD quad at the wrong depth. 3D Vision placed such elements at the depth under them.
- **Host ImGui overlays** (toasts, performance overlay, graphics menu): an optional third quad, through the SDK's detached `AppUIDrawContext`.
- **Comfort settings**, all from precedents:
  - Camera shake, G-force head motion, look-to-apex, motion blur and depth of field off.
  - Recenter and seat-position controls.
  - Configurable IPD and world scale.

### Open questions for the camera and render-loop work

| # | Question | Decides |
|---|---|---|
| 1 | Where are the cockpit camera's view and projection built? Is there a guest function or object to hook, and which vertex-shader constants carry them (axes, handedness)? | Stage 2 hook; P for `P_eye*T_eye*P^-1` in stage 4 |
| 2 | Does one frustum drive culling, LOD, shadow-cascade fitting and streaming? Can it be widened like the ultrawide hook, to the eye FOV plus about 10°? | Culling holes in stage 2 (RPCS3 VR, HindsightVR and openRBRVR all had to widen it) |
| 3 | Can the game render a near-square, asymmetric (off-axis) frustum? Is the projection built from FOV and aspect, or can a matrix be injected? | Eye raster size; stage 2 |
| 4 | Is the cockpit drawn with its own projection, near plane or depth partition? | Depth decode in stage 3; clip-shift size in stage 4 |
| 5 | How many world units make a metre, and where is the driver's eye point in each car? | IPD, world scale, seat position |
| 6 | Where do shake, G-force head motion, look-to-apex and depth of field come from, and can they be switched off? | Comfort |
| 7 | Is the camera used outside rendering (audio listener, streaming, AI)? Is it safe to change it per frame with the head pose? | Stage 2 |
| 8 | Is FM4-style Kinect head-tracking code present in FH1? `xam_nui.cpp` says so; public sources show only voice GPS. | A ready camera input whose culling already follows the head |
| 9 | Where should a frame tag attach so it follows the frame the render thread actually draws (`+0x948` skip counter)? Are VdSwap and `IssueSwap` 1:1 in every mode? | Frame tags |
| 10 | How many frames separate building the camera from the swap? | Pose prediction and the latency cap |
| 11 | Can the render thread redraw the last frame with a new camera without running the simulation? | Decoupling head rate from game rate; otherwise only GPU command replay (Dolphin VR's opcode replay) remains |
| 12 | Is the renderer forward or deferred? Which resolves feed later passes (post-processing, bloom, reflections, mirror, minimap), and which full-screen passes rebuild position from depth? | How many textures need two layers in stage 4; per-eye inverse projections |
| 13 | Do the main scene and its post-processing fit in one deferred submission per frame, or do `readback_resolve` or memexport waits split them? | Per-eye replay or multiview in stage 4 |
| 14 | Does cockpit glass write depth before the depth resolve that the velocity pass reads? | Whether stage 3 is viable |
| 15 | Does `CaptureHdrScene`'s trigger fire in cockpit view, in every race and menu mode, and at non-1280 and near-square rasters? | Patch B's eye source |
| 16 | Do HUD draws form an identifiable range after the transition, and does their blending write usable alpha? Are world-space markers drawn in the scene pass or the HUD pass? | HUD extraction method |
| 17 | Is there a clean extra-view path, such as the rear-view mirror? | Only for judging the game-draws-twice reference |

**Outside the reverse engineering (tests and checks):**
- **Patch 0038 ordering.** Does the refresher's work reach the queue before the presenter paints? The XR path avoids depending on it, but the existing presenter path deserves a check.
- **Queue sharing.** On RADV there is only one graphics queue. Does holding the shared queue lock across SteamVR or Meta `xrEndFrame` stall the submission thread?
- **New frame rates.** Do the game's simulation and pacing hold real-time speed at 24, 36 and 45 fps?
- **Runtime support for depth and motion vectors.** Which PC runtimes accept app depth and motion vectors today: Link ASW 2.0, VDXR, SteamVR? How well do Monado, WiVRn and ALVR reproject? Monado lists 3DoF timewarp; spacewarp is reported as in development.
- **Flatpak.** Do host-installed Monado or WiVRn runtime libraries load inside the sandbox? WiVRn's Flatpak build may be the only practical path.
- **GPU cost.** What is the real cost at headset resolution under the integer EDRAM stride? This needs a benchmark.
- **Player tolerance.** Do players accept the world stepping at 30-45 fps with runtime synthesis? Only a headset test can answer.

### Licences

| Project | Licence | Reuse here (MIT) |
|---|---|---|
| HindsightVR | GPLv2+ | Ideas only |
| RPCS3 VR fork | GPLv2 | Ideas only |
| Dolphin | GPLv2+ | Ideas only |
| PPSSPP | GPL | Ideas only |
| openRBRVR | MPL-2.0 | File-level copyleft |
| UEVR | No licence id reported by GitHub | Check before reusing |
| OpenXR-SDK loader | Apache-2.0 | Ship |

### Sources

OpenXR spec and runtimes:
- https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html (`XR_KHR_vulkan_enable2` initialization, concurrency and swapchain layout; `xrWaitFrame`/`xrEndFrame`; `XrCompositionLayerProjectionView`; D3D12 resource state; `XR_EXT_frame_synthesis`)
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable2.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrEndFrame.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerSpaceWarpInfoFB.html
- https://github.khronos.org/OpenXR-Inventory/runtime_extension_support.html
- https://developer.android.com/develop/xr/openxr/extensions/XR_EXT_frame_synthesis
- https://monado.pages.freedesktop.org/monado/vulkan-extensions.html
- https://fosdem.org/2026/events/attachments/T38AKM-monado-and-beyond/slides/266219/monado_an_tfzl8ak.pdf (found only through a search snippet; the link did not resolve)
- https://github.com/mbucchia/VirtualDesktop-OpenXR
- https://github.com/WiVRn/WiVRn
- https://community.khronos.org/t/display-overlay-without-submitting-frame-possible/108397
- https://forums.developer.nvidia.com/t/vkwaitforfences-does-not-return-in-a-timely-manner-with-sync-fd/373147
- https://www.pcworld.com/article/2490448/windows-11s-2024-update-finally-abandons-this-once-promising-feature.html

Reprojection and frame synthesis:
- https://developers.meta.com/horizon/blog/developer-guide-to-asw-20/
- https://developers.meta.com/vr/documentation/native/pc/asynchronous-spacewarp/
- https://www.meta.com/blog/introducing-asw-2-point-0-better-accuracy-lower-latency/
- https://developers.meta.com/horizon/documentation/native/android/mobile-asw/
- https://roadtovr.com/steamvr-motion-smoothing-asw-alex-vlachos/
- https://roadtovr.com/steamvr-update-brings-motion-smoothing-easier-keybinding/
- https://bit-tech.net/news/valve-launches-steamvr-motion-smoothing-beta/1
- https://uploadvr.com/reprojection-explained/
- https://uploadvr.com/virtual-desktop-synchronous-spacewarp/
- https://developers.meta.com/vr/blog/hybrid-mono-rendering-in-ue4-and-unity/
- https://www.lgdv.tf.fau.de/files/2019/07/HybridVR-reduced.pdf

Flat games in VR:
- UEVR: https://github.com/praydog/UEVR, http://docs.uevr.io/usage/overview.html, https://github.com/praydog/UEVR/blob/master/src/mods/vr/FFakeStereoRenderingHook.cpp, https://github.com/hatrd/UEVR
- Luke Ross R.E.A.L.: https://uploadvr.com/rdr2-vr-mod-not-soon/, https://www.patreon.com/realvr/posts/aer-v2-152398605, https://mixed-news.com/en/real-vr-mod-dlss-ray-reconstruction/, https://mixed-news.com/en/kingdom-come-deliverance-ii-vr-mod/
- vorpX: https://www.vorpx.com/more-headtracking-z-buffer-vs-geometry-3d/, https://www.vorpx.com/features/, https://thelatenightsession.com/2019/08/03/2d-games-in-3d-via-vr-virtual-desktop-and-vorpx/
- NVIDIA 3D Vision Automatic: https://archive.docs.nvidia.com/gameworks/content/technologies/desktop/nv3dva_background.htm, https://archive.docs.nvidia.com/gameworks/content/technologies/desktop/nv3dva_stereoscopic_issues.htm
- Dolphin: https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/GeometryShaderGen.cpp, https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/GeometryShaderManager.cpp, https://dolphin-emu.org/blog/2015/05/13/a-second-perspective/, https://forums.dolphin-emu.org/Thread-stereoscopic-3d, https://github.com/CarlKenner/dolphin/blob/VR-Hydra/Source/Core/VideoCommon/VR.cpp, https://forums.dolphin-emu.org/Thread-oculus-rift-support, https://www.generationamiga.com/2026/07/06/dolphinxr-v0-3-improves-gamecube-and-wii-vr-emulation-with-major-metroid-prime-fixes/
- PPSSPP: https://github.com/hrydgard/ppsspp/blob/master/Common/VR/PPSSPPVR.cpp, https://www.ppsspp.org/docs/reference/vr-apk
- CitraVR: https://mixed-news.com/en/meta-quest-citravr-nintendo-3ds-emulator/
- RPCS3 and the VR fork: https://dsogaming.com/?p=136128, https://github.com/hookmanuk/rpcs3, https://github.com/hookmanuk/rpcs3/releases, https://github.com/hookmanuk/rpcs3/blob/openxr/RPCS3_VR_FORK_STARTING_POINT.md, https://github.com/hookmanuk/rpcs3/blob/openxr/vr-games.md, https://github.com/hookmanuk/rpcs3/blob/openxr/bin/vr_profiles/BCUS98114.json
- PenguinScreen2: https://www.generationamiga.com/2026/08/16/penguinscreen2-adds-vr-head-tracking-to-ace-combat-5-and-more-ps2-games/
- HindsightVR: https://gitlab.com/aknumbers/hindsightvr, https://gitlab.com/aknumbers/hindsightvr/-/blob/dev/flow.md, https://gitlab.com/aknumbers/hindsightvr/-/blob/dev/docs/adapters/xenia/halo3.md, https://gitlab.com/aknumbers/hindsightvr/-/blob/dev/docs/methods/emulator-render-path.md
- BotW BetterVR: https://github.com/Crementif/BotW-BetterVR, https://roadtovr.com/zelda-breath-of-the-wild-vr-mod/
- openRBRVR: https://github.com/Detegr/openRBRVR, https://github.com/Detegr/openRBRVR/blob/master/FAQ.md, https://github.com/Detegr/openRBRVR/blob/master/src/RBR.cpp, https://github.com/Detegr/openRBRVR/blob/master/src/Dx.cpp, https://github.com/Detegr/openRBRVR/releases
- Euro Truck Simulator 2: https://www.engadget.com/2013-10-24-euro-truck-simulator-2-shifts-gears-to-add-oculus-rift-support.html, https://wiki.vronlinux.org/docs/games/ets2-ats/
- MSFS: https://forums.flightsimulator.com/t/why-does-the-rdrthread-double-in-vr/766129
- Star Wars: Squadrons: https://www.ea.com/studios/motive/news/the-making-of-vr-part-2
- Single-pass stereo: https://developer.nvidia.com/vrworks/graphics/vrsli, https://docs.unity3d.com/2019.1/Documentation/Manual/SinglePassInstancing.html, https://saschawillems.de/blog/2018/06/08/multiview-rendering-in-vulkan-using-vk_khr_multiview/, https://docs.kdab.com/kdgpu/unstable/multiview.html
- CryEngine 3 and depth-based stereo: https://mcvuk.com/key-release-the-3d-powered-cryengine-3-2/, https://www.tomshardware.com/reviews/tridef-stereoscopic-3d-gaming%2C3019-10.html, https://www.ee.cityu.edu.hk/~lmpo/publications/2012_ICASSP_Depth.pdf
- Comfort and rest frames: https://www.ncbi.nlm.nih.gov/pmc/articles/PMC9762595/, https://www.faac.com/blog/2021/02/17/simulator-sickness-design-implications-of-the-rest-frame-hypothesis/, https://paper.nweon.com/15302
- Forza Kinect: https://www.engadget.com/2012-06-06-forza-horizon-to-feature-voice-activated-kinect-gps.html, https://www.autoguide.com/auto-news/2011/06/forza-motorsport-4-features-head-tracking-via-kinect-video.html

This repo:
- Docs: [perf.md](perf.md), [framerate.md](framerate.md), [rendering.md](rendering.md), [native_dlss.md](native_dlss.md), [hdr.md](hdr.md), [ultrawide.md](ultrawide.md), [sdk_patches.md](sdk_patches.md)
- Game code: [vulkan_hooks.cpp](../forza-recomp/src/platform/vulkan_hooks.cpp), [frame_pacing.h](../forza-recomp/src/platform/frame_pacing.h), [frame_time.cpp](../forza-recomp/src/hooks/frame_time.cpp), [ultrawide_projection.cpp](../forza-recomp/src/hooks/ultrawide_projection.cpp), [xam_nui.cpp](../forza-recomp/src/kernel/xam_nui.cpp)
- SDK patches: [0057](../patches/rexglue-sdk/0057-dlss-scene-jitter-coverage.patch), [0058](../patches/rexglue-sdk/0058-dlss-reprojection-and-tile-jitter.patch)
- Flatpak manifest: [the Flatpak manifest](../flatpak/io.github.dyotson.ForzaHorizon1ReImagined.yml)
- SDK sources, in the checkout the build uses: `src/ui/vulkan/vulkan_instance.cpp`, `vulkan_provider.cpp`, `vulkan_device.cpp`, `vulkan_presenter.cpp`, `src/ui/presenter.cpp`, `src/graphics/vulkan/command_processor.cpp` (`IssueSwap`, `CaptureHdrScene`, `EndSubmission`), `src/graphics/graphics_system.cpp` (GPU VSync worker), `src/ui/d3d12/d3d12_provider.cpp`, `include/rex/graphics/vulkan/deferred_command_buffer.h`

## Answers to the research questions

Numbers refer to the table above.

1. **View and projection.** The view is built by the director update (`sub_82858288`) into
   director + 240 (row-vector, left-handed, x right, y up, z forward). `sub_825A9D20` hands
   it to the presentation camera with SetViewTransform, and the projection's inputs with
   SetFOV, SetAspectRatio and SetZClip; the projection itself is built on the render side.
   The hook point is the prototype's. The GPU gets per-object products the render thread
   computes (`matWVP`, `matWV`, `matVP`, `matVInv` in the car shaders, `CamViewProjMat`,
   `CamProjMat`, `PCamViewProjMat` in the engine's), not a view constant; their register
   numbers per shader are not mapped. P can be rebuilt from FOV, aspect and clip planes, or
   read from the render-side `CPresentationCamera` + 80.
2. **One frustum?** The view matrix has two readers: the presentation camera (render-side
   culling, shadows, mirrors) and the game-side frustum `sub_829A4860`. Both follow the
   prototype's pose; nothing was missing at 60-160 degrees, and posing the game-side one
   or not makes no visible difference. Both take their aspect from `sub_823E22C8`, which
   the ultrawide hook already widens, and the FOV from director + 304, so a margin for
   reprojection is a scale in the same hook. Level of detail is distance-based
   (`CarLODTransDist` etc. in the render scenarios) and barely moves with a head offset.
3. **Off-axis, near-square.** By default the projection is symmetric, from FOV and aspect.
   The interface also has SetProjectionTransform (+172, a full matrix; used by the UI's
   scene cameras, `sub_82DBDE80`) and `InitPerspectiveObliqueCamera`; whether the main view
   honours an injected matrix is not tested. Taller-than-16:9 rasters are untested.
4. **Cockpit projection.** One near/far pair per camera; the in-car camera's near plane is
   0.1 m (`Driver\CamNearClip`), FOV 62 degrees. No separate interior camera or pass in
   the camera code; a renderer-side depth partition is not checked.
5. **Units, eye point.** Metres: the chase camera sits 5.1 m from the car's position, and
   `CameraSettings.ini` gives offsets in metres. The eye is
   `Driver\DefaultOffsetFromSteeringWheel` (0, 0.21, -0.51) m from the steering wheel, plus
   the car's `CamCockpitOffsetX/Y/Z`; the result is the director's position (+176) each
   frame.
6. **Shake, G-forces, depth of field.** Camera physics layers in `CameraPhysics.xml` map
   input channels (an enum at `0x82074100`: `Gs_Lateral`, `Gs_Longitudinal`, collisions,
   `Slip`, `Speed`, `RPM`, `SurfaceBumpOffset_*`, `ShoulderYaw/Roll`, `HeadHomeOffset*`,
   `HeadTracking*`, ...) through curves and springs to `CarSpace`/`CameraSpace` XYZ/YPR
   offsets. `CameraPhysicsSansEffects.xml` is the same without effects, most likely what the
   player's Camera Effects option (Default / Vignette / Off) selects: not verified. Depth of
   field and motion blur are render-scenario switches (`DepthOfField`, `MotionBlur`).
7. **Camera outside rendering.** The prototype poses only the matrix; the director's
   position, forward and up (+176/+208/+224) stay the car's, so whatever reads those (the
   sound listener has `lockListenerToCamFOV`; distances) sees the unposed camera. Changing
   the pose every frame is safe: nothing integrates it, the matrix is put back after each
   reader, and 15 pose changes and 21 reader switches during a race left no trace in the log.
8. **Kinect head tracking.** Present: the option `IDS_OptionHeadTracking` (On/Off),
   `ForzaProfileHeadTrackingSettings`, `ForzaUI::CHeadTrackingSetup`, `CNatalCameraManager`,
   and camera-physics input channels 27-31 (`HeadTrackingX/Y/Z/Yaw/Pitch`). In FH1's
   `CameraPhysics.xml` their layers are disabled (`Enabled="0"`) and amplify through curves
   and springs (25 degrees of head yaw become about 9 degrees of camera yaw): made for a TV.
   VR keeps the direct matrix pose.
9. **Frame tags.** The camera is built and handed over in the world update on the main loop
   thread (`sub_823E3218` > `sub_825AA1B0` > `sub_825A9D20`); the render thread draws it
   later from the deferred queue. The tag belongs in the `sub_825A9D20` hook: the pose that
   frame used. VdSwap against `IssueSwap` is not checked.
10. **Camera to swap.** Not measured. At least the main loop to render thread hand-over (one
   frame of queue); stamping a pose and finding it at VdSwap would measure it.
11. **Redraw with a new camera.** No entry point found. The render-side presentation camera
   holds the matrix (+16, dirty flag +464), but re-running the render thread's frame without
   a new frame from the main loop is not a path the game has.
12. **Renderer.** From [rendering.md](rendering.md): the scene is 4x MSAA in three EDRAM
   tiles (predicated tiling), post-processing runs once at the end, the velocity pass reads
   the resolved depth and stencil, mirrors are extra camera views (CCamRearView). A free-roam
   frame is about 3,000 host draws: the scene's three tiles 1,969, shadow cascades about
   330, another depth-only pass 388, post-processing about 250 (table in
   [Stereo](#stereo-drawing-the-scene-twice)). Forward or deferred is not determined here.
13. **One submission per frame?** Not checked.
14. **Glass and depth.** Not checked.
15. **`CaptureHdrScene` in the cockpit.** Not checked directly; the same front-buffer
   transition separated scene and HUD in every cockpit screenshot's frame.
16. **HUD range and alpha.** The range is identifiable (everything after the transition);
   whether its blending writes usable alpha is not checked. No world-space markers were in
   the screenshots' frames.
17. **Extra-view path.** Yes: the second split-screen view, and the mirror cameras.

## Recommended plan

The research's four stages, mapped to the three asked for. Sizes are rough: S is days,
M one to two weeks, L a month or more of focused work.

**(a) Head tracking on a flat virtual screen**

1. OpenXR session and a virtual screen (research stage 1): SDK patch A (Vulkan created
   through the runtime), the XR thread, a quad showing the guest output, the window as a
   mirror, loader and Flatpak permissions, a launcher toggle. **M.** Works on every PC,
   and stays the mode for menus, loading, movies and slow CPUs.
2. Head-tracked mono cockpit (research stage 2): the prototype's hook reading the XR pose
   slot instead of the FIFO (**S**, the hook point and culling are done), FOV/aspect widened
   to the eye FOV plus a margin through the same hook (**S**), the pre-HUD scene and the HUD
   exported per swap (patch B, **M**), guest vblank phase-locked to the headset (patch C,
   **S-M**), frame tags (**S**), the HUD on a quad. Gate: is a mono cockpit at 30 fps with
   runtime reprojection comfortable enough to ship as an option?

**(b) Stereo**

3. Depth reprojection of the mono view to two eyes (research stage 3), using the DLSS/FSR 3
   depth decode. **M.** CPU flat. First check that cockpit glass does not write depth.
4. Before or beside it, a cheap reference: put the second split-screen view to use as the
   other eye on a fast PC (**S-M** to find out, larger if the split-screen renderer needs a
   second player). Gives correct stereo to compare stage 3 against and measures what the
   game drawing twice really costs.
5. True stereo inside the GPU emulation (research stage 4: multiview or per-eye replay).
   **L.** Only after 3 has shipped and its artifacts are known.
6. Worth trying early because it pays for any of these and helps flat play: drawing the
   main scene without predicated tiling (one pass instead of three for the command
   processor). **S** to try, **M** if it needs SDK work.

**(c) Comfort and UI**

7. Camera effects off (shake, G-force head motion: the effects-free camera physics), motion
   blur and depth of field off, the game's look-around and look-back disabled while
   tracked, recenter and seat position, IPD and world scale. **S-M.**
8. HUD on a panel (cleared UI target with alpha, placement), menus and pause on the cinema
   quad, the player's missing body (an option to hide the arms, or accept it). **M.**

Order: 1, 2, then 7 with it (comfort is needed as soon as the head moves the camera), 3, 8,
and 4 and 6 in parallel with 3. 5 last.

## Open questions

- Can the main view run non-tiled (the split-screen `TilingScenario 4`), or can the SDK
  skip the redundant predicated tiles with host render targets? That would cut the command
  processor's scene work to about a third.
- What a second view really costs: a two-player split-screen race against a one-player race,
  CPU per thread and GPU per frame, if FH1's menus still reach split-screen (player 2 can be a
  guest; the scripted input needs a persistent second pad, `FORZA_PAD_PER_USER=1`).
- Can a split-screen view exist without a second player and car, and can its target be a
  full-size eye image?
- Does the main view take an injected off-axis projection (SetProjectionTransform, or
  `InitPerspectiveObliqueCamera`), and does a near-square raster work with the EDRAM tiling?
- What does the game-side frustum (`sub_829A4860` > `sub_829A7448`) decide? Nothing visible
  changed with it posed or not.
- How many frames lie between the camera's hand-over and the swap?
- Does the HUD's blending leave usable alpha on a cleared target?
- Which camera physics file does the Camera Effects option pick, and does it remove all of
  the in-car motion?
- The research's own list (runtime depth support, queue sharing, new frame rates, Flatpak,
  GPU cost at headset resolution) still applies.
