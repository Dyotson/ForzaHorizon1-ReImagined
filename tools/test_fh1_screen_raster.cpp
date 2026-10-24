// No game data or GPU required. Pass the patched SDK include directory to the compiler.
#include <rex/graphics/vulkan/fh1_screen_raster.h>
#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
  using rex::graphics::vulkan::fh1::IsDisplayViewport;
  // Display, post-processing and cinematic viewport scales, with either Y sign.
  for (const auto x : {640.0f, 320.0f, 160.0f, 80.0f, 40.0f}) {
    for (const auto sign : {-1.0f, 1.0f})
      assert(IsDisplayViewport(true, true, x, sign * x * 9.0f / 16.0f, 8192, 8192));
  }
  assert(IsDisplayViewport(true, true, 640, -232, 1280, 464));
  // Reflection faces and shadow maps must not inherit a 0.75 display scale.
  // A 256-pixel face at 2x must cover all 512 pixels, rather than only 384.
  for (const auto half_extent : {64.0f, 128.0f, 256.0f, 512.0f, 2048.0f}) {
    for (const auto sign : {-1.0f, 1.0f})
      assert(!IsDisplayViewport(true, true, half_extent, sign * half_extent, 8192, 8192));
  }
  // Software viewports require the explicitly bounded display rectangle.
  for (const bool x_enabled : {false, true}) {
    for (const bool y_enabled : {false, true}) {
      if (x_enabled && y_enabled) continue;
      assert(IsDisplayViewport(x_enabled, y_enabled, 1, 1, 1280, 720));
      assert(!IsDisplayViewport(x_enabled, y_enabled, 1, 1, 256, 256));
      assert(!IsDisplayViewport(x_enabled, y_enabled, 1, 1, 8192, 8192));
    }
  }
  assert(IsDisplayViewport(true, true, 0, 0, 1280, 720));
  std::puts("FH1 display/off-screen viewport policy passed");
}
