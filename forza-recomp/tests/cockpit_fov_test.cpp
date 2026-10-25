#include "../src/platform/cockpit_fov.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>

float Decode(const uint8_t* p) {
  uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

int main() {
  using forza::camera::ScopedCockpitFov;
  using forza::camera::ValidCockpitFov;
  std::array<uint8_t, 0x2C> world{};
  assert(!forza::camera::LiveCockpitWorld(nullptr));
  assert(!forza::camera::LiveCockpitWorld(world.data()));
  world[0x0D] = 1;
  world[0x28] = 0x3F; world[0x29] = 0x80; // Loaded, forward simulation at 1x.
  assert(forza::camera::LiveCockpitWorld(world.data()));
  world[0x0C] = 1; // Pause/replay/rewind menu flag, even at positive time scale.
  assert(!forza::camera::LiveCockpitWorld(world.data()));
  world[0x0C] = 0; world[0x28] = 0xBF; // Active rewind at -1x.
  assert(!forza::camera::LiveCockpitWorld(world.data()));
  world[0x28] = 0; world[0x29] = 0; // Stopped simulation.
  assert(!forza::camera::LiveCockpitWorld(world.data()));
  // Guard bytes represent adjacent director state; exact bytes survive each reader.
  std::array<uint8_t, 8> memory{0xAB, 0xCD, 0x3F, 0x8A, 0x82, 0x0C, 0xEF, 0x12};
  const auto stock = memory;
  for (int degrees : {0, -1, 1, 39, 91, 1000}) {
    ScopedCockpitFov scope(memory.data() + 2, degrees, true, false);
    assert(memory == stock);
  }
  assert(ValidCockpitFov(0) && !ValidCockpitFov(39) && !ValidCockpitFov(91));
  for (int degrees = 40; degrees <= 90; ++degrees) {
    assert(ValidCockpitFov(degrees));
    float presentation = 0;
    for (int reader = 0; reader < 2; ++reader) {
      {
        ScopedCockpitFov scope(memory.data() + 2, degrees, true, false);
        const float radians = Decode(memory.data() + 2);
        assert(std::fabs(radians - float(degrees) * 3.14159265358979323846f / 180) < 1e-6f);
        if (reader == 0) presentation = radians;
        else assert(radians == presentation);
        assert(memory[0] == stock[0] && memory[1] == stock[1]);
        assert(memory[6] == stock[6] && memory[7] == stock[7]);
      }
      assert(memory == stock);
    }
  }
  { ScopedCockpitFov scope(memory.data() + 2, 90, false, false); assert(memory == stock); }
  { ScopedCockpitFov scope(memory.data() + 2, 90, true, true); assert(memory == stock); }
  { ScopedCockpitFov scope(nullptr, 90, true, false); }
  {
    ScopedCockpitFov scope(memory.data() + 2, 90, true, false);
    memory[4] = 0; // A newer guest update must survive scope destruction.
  }
  assert(memory != stock && memory[4] == 0);
}
