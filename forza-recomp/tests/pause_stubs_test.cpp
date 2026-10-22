// A normal build must link and call the optional pause APIs without game/SDK code.
#include "../src/platform/fh1_map_preview.h"
#include "../src/platform/fh2_pause_scene.h"
#include "../src/platform/pause_menu.h"
#include <array>
#include <cassert>

// Disabled implementations must never inspect the guest context or memory.
struct PPCContext {};

int main() {
  PPCContext context;
  std::array<uint8_t, 16> memory{};
  memory.fill(0xA5);
  const auto before = memory;
  for (uint32_t screen : {0u, 1u, 0x827D1618u}) {
    assert(!forza::fh2_pause_scene::Owns(screen));
    assert(!forza::fh2_pause_scene::Input(context, memory.data()));
    assert(!forza::fh2_pause_scene::Page(context, memory.data(), screen, true));
    forza::fh2_pause_scene::Destroy(context, memory.data(), screen);
    forza::pause_menu::Bind(context, memory.data());
    forza::pause_menu::AfterBind(context, memory.data());
    forza::pause_menu::Tick(context, memory.data());
    assert(!forza::pause_menu::Select(context, memory.data()));
    for (bool visible : {false, true}) {
      forza::pause_menu::Page(memory.data(), screen, visible);
      forza::fh1_map_preview::Update(context, memory.data(), screen, visible);
    }
    forza::fh1_map_preview::Release(context, memory.data());
    assert(!forza::fh1_map_preview::Ready());
    assert(memory == before);
  }
}
