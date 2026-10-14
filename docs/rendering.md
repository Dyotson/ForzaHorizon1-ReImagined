# Rendering issues

## Open: hard-edged rectangle in the speed blur

**Symptom.** At speed the game blurs the edges of the screen. A rectangle around the
player's car is never blurred, and its edge is sharp; on the console the blur fades in
smoothly toward the car.

**State (2026-10-14).** Not fixed; the pass that makes the rectangle is found. New
captures in the Millennia Masters race (TU4 build, 60 fps, full throttle, about 14 s after
GO), analysed with qrenderdoc Python scripts (draw list with shader hashes, targets,
viewports; texture saves; stencil state of every draw; pixel history).

- The scene is drawn 4x MSAA in three EDRAM tiles (rows 0-255, 256-511, 512-719); the
  post-processing runs once at the end of the frame.
- Velocity pass (`PPMotionBlurDepthHorizonVel`): one full-screen rectangle into a 640x360
  8888 target. It reads the scene depth resolved to an 8888 texture (byte 0 = stencil,
  bytes 1-3 = 24-bit depth) with point sampling, and picks the reprojection matrix by the
  stencil value (relative constant addressing, `c[32+a0]`). Pixels with stencil 15 (the
  player's car) or 128 get zero velocity (stored as 127,127 with alpha 255).
- A second 640x360 pass (two taps) barely changes it (1,659 of 230,400 pixels differ).
- Combine (`BloomTonemap_MotionBlur_Vignette`, drawn as two 1280x360 halves): reads the
  velocity linearly, takes five scene taps along it and rejects a tap whose scene alpha
  differs from the centre's, then bloom, a 3D colour LUT and gamma. No scissor, mask
  texture or rectangle of its own: the sharp edge is the edge of the zero-velocity area.
- The zero-velocity area is the car (stencil 15) plus a trapezoid on the ground around it
  with stencil 128: a ground-plane rectangle seen in perspective. Its only writer is one
  rectangle-list draw per tile (pixel shader `39ed63c1`, depth test and write on, stencil
  always/replace 128, blend src-alpha/inv-src-alpha, colour mask RGB, cull none). Alpha
  test is off for it (system flags 0x0007cc00: alpha function "always"), and the translated
  shader has no kill, so it marks its whole area.
- Ruled out: a scissor or viewport on the blur draws, point filtering of the velocity, a
  mask texture, the resolve region of the velocity target.
- Open question: what limits that stencil-128 area on the console (the guest pixel
  shader's ucode, a kill or alpha test the SDK drops, or the rectangle's 4th vertex
  computed differently for a perspective rectangle list). Xenia was not compared: it only
  takes pad input on Linux, and a global virtual pad is not allowed.
- Screenshots (`FORZA_SCREENSHOT_TRIGGER`) still come out empty under RenderDoc; run a
  second pass without it for pictures.

**What the code shows.**
- `sub_82461108` draws the technique `PPMotionBlurDepthHorizonVel` (a velocity pass from
  depth): a full-screen quad given as normalized shorts (-32767..32767).
- `sub_82402678` uses `BloomTonemap_MotionBlur_Vignette`, probably the final combine that
  applies the blur.
- The camera definitions set a `MotionBlurAmount` per shot (`media/camera.zip`, for
  example `321Cams.xml`).

**Next steps.**
1. Capture again at speed. List the draws (RecompiledHaloReach `tools/renderdoc/draws.py`)
   and find the combine draw that samples the velocity target.
2. Save its inputs (`savetex.py`), and read the blend state, scissor, stencil and viewport
   of the blur draws. Look for what cuts the rectangle: a mask texture, a scissor or
   stencil rectangle, a resolve region, a sampler (point instead of linear filtering), a
   format, or the 4th vertex of a rectangle-list primitive.
3. Compare with Xenia. Its input must stay inside a nested display: never a global
   uinput pad, which Steam could map onto the desktop.
4. Fix it in a game hook, or as SDK patch 0040 or later, in both projects'
   `patches/rexglue-sdk/`.
