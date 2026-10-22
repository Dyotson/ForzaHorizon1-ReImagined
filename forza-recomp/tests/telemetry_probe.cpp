// Windows native producer probe: sparse, synthetic guest memory, no game data.
// Link with platform/telemetry.cpp, rexruntime and ws2_32, then receive FH1T UDP.
#include "../src/platform/telemetry.h"
#include "../src/platform/guest_addr.h"
#include <windows.h>
#include <bit>
#include <cstdlib>
#include <cstdint>
#include <cstring>

int main(int argc, char**) {
  auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
  if (!base) return 1;
  auto word = [&](uint32_t address, uint32_t value) {
    if (!VirtualAlloc(base + (address & ~0xfffu), 4096, MEM_COMMIT, PAGE_READWRITE)) std::abort();
    value = std::byteswap(value);
    std::memcpy(base + address, &value, 4);
  };
  const uint32_t root = 0x1000000, holder = 0x1010000, list = 0x1020000,
                 slots = 0x1030000, vehicle = 0x1040000, car = 0x1060000;
  word(FORZA_ADDR(0x832DF024), root); word(root+4, holder); word(holder+4, list);
  word(list+0x80, slots); word(list+0x84, slots+4); word(slots, vehicle);
  word(vehicle+0x2DD0, 0); word(vehicle+0x2D90, car);
  auto position = [&](float x) {
    for (int i = 0; i < 3; ++i) {
      word(car-0xA20+i*4, std::bit_cast<uint32_t>(i == 0 ? x : 0.f));
      word(car-0xA40+i*4, std::bit_cast<uint32_t>(i == 2 ? 1.f : 0.f));
    }
  };
  for (int step = 0; step < 6; ++step) {
    Sleep(25);
    position(step < 3 ? step*.1f : 10000.f); // Teleport resets derivatives.
    if (step == 4) word(vehicle+0x2DD0, 2); // Remote car must not be sampled.
    if (step == 5) word(FORZA_ADDR(0x832DF024), 0x70000000); // Unmapped pointer.
    forza::telemetry::Rumble(0, 1000);  // driving: the game's rumble is running
    forza::telemetry::Input(step != 3, 255, 128, 0x1000, -32768);
    forza::telemetry::Physics(base);
  }
  if (argc > 1) {
    word(FORZA_ADDR(0x832DF024), root);
    word(vehicle+0x2DD0, 0);
    for (int step = 0; step < 20; ++step) {
      Sleep(step == 10 ? 220 : 25); // Queued commands must expire across a pause.
      position(0);
      forza::telemetry::Rumble(0, 1000);
      forza::telemetry::Input(true, 255, 128, 0, 0);
      forza::telemetry::Physics(base);
    }
  }
  VirtualFree(base, 0, MEM_RELEASE);
}
