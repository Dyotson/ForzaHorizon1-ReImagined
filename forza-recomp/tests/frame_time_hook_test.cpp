// Windows native hook regression, no game files/GPU. Compile with the patched SDK,
// /std:c++latest /O2 /Gy /MD /D_USE_STD_VECTOR_ALGORITHMS=0, link rexruntime.lib /OPT:REF.
// Define FORZA_TITLE_UPDATE=0 and 4 in separate runs. See docs/framerate.md.
#ifndef _WIN32
#error This sparse guest-memory fixture uses Windows VirtualAlloc.
#endif
#include "../src/hooks/frame_time.cpp"
#include <windows.h>
#include <cassert>

REXCVAR_DEFINE_INT32(frame_rate, 30, "Test", "Native frame-time hook regression");
namespace forza::stall { void OnMainFrame() {} }
namespace forza::graphics_menu { void Tick(PPCContext&, uint8_t*) {} }
namespace forza::benchmark { void Tick(PPCContext&, uint8_t*) {} }

namespace {
unsigned game_count = 0, releases = 0;
}

// Only the original counter/event is modeled; the divider-option writes and the
// forwarded/skipped vblanks execute the actual submitted hook above.
extern "C" REX_FUNC(__imp__sub_8247C678) {
  if (!*forza::GuestPtr(base, kMainThread30Fps) || ++game_count == 2) {
    game_count = 0;
    ++releases;
  }
}
extern "C" REX_FUNC(__imp__sub_8259F3E8) {}
extern "C" REX_FUNC(__imp__sub_8247C168) {}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  rex::chrono::Clock::set_guest_tick_frequency(50000000);
  auto* base = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
  assert(base);
  for (uint32_t address : {kOptionsGuard, kMainThread30Fps}) {
    assert(VirtualAlloc(base + (address & ~0xfffu), 4096, MEM_COMMIT, PAGE_READWRITE));
  }
  *forza::GuestPtr(base, kOptionsGuard + 3) = 1;
  *forza::GuestPtr(base, kMainThread30Fps) = 1;
  PPCContext ctx{};
  auto run = [&](const char* fps, unsigned vblanks, unsigned expected) {
    game_count = releases = 0;
    assert(rex::cvar::SetFlagByName("frame_rate", fps));
    for (unsigned i = 0; i < vblanks; ++i) sub_8247C678(ctx, base);
    std::printf("rate %s: %u releases in %u vblanks (expected %u), divider option %u\n",
                fps, releases, vblanks, expected, *forza::GuestPtr(base, kMainThread30Fps));
    assert(releases == expected);
  };
  *forza::GuestPtr(base, kMainThread30Fps) = 0;  // state left by a one-vblank rate
  run("20", 300, 100);
  run("40", 300, 100);
  run("60", 300, 150);
  for (const char* rate : {"20", "40"}) {
    *forza::GuestPtr(base, kMainThread30Fps) = 0;  // option parser disabled it
    run(rate, 300, 100);
  }
  run("30", 300, 150);
  assert(VirtualFree(base, 0, MEM_RELEASE));
  std::puts("native frame-time hook: disc/TU4 divider transitions pass");
}
