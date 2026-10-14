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

