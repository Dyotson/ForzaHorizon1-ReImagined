// Windows probe of the production Poll and live guest-world guards. Focus and
// disconnect notifications are injected; this does not claim an OS focus or USB test.
#include <windows.h>
#include "../src/platform/auto_pause.cpp"
#include <cassert>
#include <cstdio>
#ifdef FORZA_AUTO_PAUSE_LEGACY_SDK
#include "../src/platform/sdk_import.cpp"
#endif

namespace {
uint32_t sequence = 0;
bool xr_running = false;
uint32_t Sequence(uint32_t) { return sequence; }
}
namespace forza::xr {
bool SessionRunning() { return xr_running; }
}
#ifndef FORZA_AUTO_PAUSE_LEGACY_SDK
namespace forza {
void* SdkSymbol(const char*) { return reinterpret_cast<void*>(Sequence); }
}
#endif
int main() {
  using namespace forza;
  auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x110000,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  assert(base);
  auto* world = base + 0x100000;
  world[0x0d] = 1; world[0x28] = 0x3f; world[0x29] = 0x80;  // loaded, time scale 1
  auto_pause::World(0x100000);
  telemetry::WorldFrame frame; frame.stepped = true;
  for (uint64_t ms : {1u, 300u, 600u}) telemetry::Player().Frame(ms, frame);
  assert(telemetry::Player().State() == telemetry::Drive::kDriving);
  REXCVAR_SET(forza_pause_on_focus_loss, true);
  REXCVAR_SET(forza_pause_on_controller_disconnect, true);
  assert(!auto_pause::Poll(base, 0, 15));
#ifdef FORZA_AUTO_PAUSE_LEGACY_SDK
  // This executable runs in a fresh process against the actual pinned r2 DLL. Its missing
  // optional export must leave focus working and must never manufacture a disconnect event.
  assert(!SdkSymbol("ForzaInputDisconnectSequence"));
  ++sequence;
  assert(!auto_pause::Poll(base, 0, 15));
  ++auto_pause::focus_losses;
  assert(auto_pause::Poll(base, 0, 15));
  auto_pause::Cancel();
  REXCVAR_SET(forza_pause_on_focus_loss, false);
  ++auto_pause::focus_losses;
  ++sequence;
  assert(!auto_pause::Poll(base, 0, 15));
  REXCVAR_SET(forza_pause_on_focus_loss, true);
  xr_running = true;
  ++auto_pause::focus_losses;
  assert(!auto_pause::Poll(base, 0, 15));
  xr_running = false;
  assert(!auto_pause::Poll(base, 0, 15));
  auto_pause::World(0);
  VirtualFree(base, 0, MEM_RELEASE);
  std::fprintf(stderr, "legacy SDK auto-pause: actual missing export, focus, disconnect fallback and XR guards pass\n");
  return 0;
#endif
  xr_running = true;
  ++auto_pause::focus_losses;
  assert(!auto_pause::Poll(base, 0, 15));  // VR consumes focus loss without pausing
  ++sequence;
  assert(auto_pause::Poll(base, 0, 15));  // physical disconnect remains active in VR
  auto_pause::Cancel();
  xr_running = false;
  assert(!auto_pause::Poll(base, 0, 15));  // leaving VR does not replay its focus edge
  ++auto_pause::focus_losses;
  assert(auto_pause::Poll(base, 0, 15));
  world[0x0c] = 1;  // game has paused, debounced gate still says driving
  assert(!auto_pause::Poll(base, 0, 15));
  ++sequence;
  assert(!auto_pause::Poll(base, 0, 15));  // a new event cannot resume the paused game
  world[0x0c] = 0;
  assert(!auto_pause::Poll(base, 0, 15));  // returning/reconnecting is not a request
  ++sequence; assert(auto_pause::Poll(base, 0, 15));
  assert(!auto_pause::Poll(base, 0x10, 15));  // manual START takes priority
  assert(!auto_pause::Poll(base, 0, 15));
  world[0x28] = 0xbf;  // rewind
  ++auto_pause::focus_losses; assert(!auto_pause::Poll(base, 0, 15));
  world[0x28] = 0x3f; world[0x0d] = 0;  // loading
  ++sequence; assert(!auto_pause::Poll(base, 0, 15));
  world[0x0d] = 1;
  DWORD previous;
  assert(VirtualProtect(world, 0x1000, PAGE_NOACCESS, &previous));
  ++sequence; assert(!auto_pause::Poll(base, 0, 15));  // stale/unmapped world is safe
  assert(VirtualProtect(world, 0x1000, previous, &previous));
  assert(!auto_pause::Poll(base, 0, 15));
  auto_pause::World(0);
  VirtualFree(base, 0, MEM_RELEASE);
  std::fprintf(stderr, "native auto-pause: live paused/loading/rewind/unmapped guards and manual resume pass\n");
}
