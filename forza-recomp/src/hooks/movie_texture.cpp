// The WMV player explicitly cleans its CPU-written Y/U/V planes before drawing.
// Recompiled dcbst instructions are no-ops, so forward those clean operations to
// the renderer's physical-memory watches. In particular, buffers reused after
// gameplay may otherwise retain an old GPU copy (black or purple/white video).
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {
bool MoviePlaneFlush(uint32_t caller) {
#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4
  return caller == 0x82F1FA60 || caller == 0x82F1FA78 || caller == 0x82F1FA90;
#else
  return caller == 0x82E5DE28 || caller == 0x82E5DE40 || caller == 0x82E5DE58;
#endif
}
void FlushPlane(uint32_t caller, uint32_t address, uint32_t length) {
  static const bool enabled = [] {
    const char* value = std::getenv("FORZA_MOVIE_CACHE_FLUSH");
    return !value || std::strcmp(value, "0") != 0;
  }();
  if (!enabled || !length || !MoviePlaneFlush(caller)) return;
  auto* memory = rex::system::kernel_state()->memory();
  memory->TriggerPhysicalMemoryCallbacks(
      rex::thread::global_critical_region::AcquireDirect(), address, length,
      /*is_write=*/true, /*unwatch_exact_range=*/true);
}
}

#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4
extern "C" REX_FUNC(__imp__sub_82D7CCE0);
extern "C" REX_FUNC(sub_82D7CCE0) {
#else
extern "C" REX_FUNC(__imp__sub_82CBB1F8);
extern "C" REX_FUNC(sub_82CBB1F8) {
#endif
  const uint32_t caller = uint32_t(ctx.lr), address = ctx.r3.u32, length = ctx.r4.u32;
#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4
  __imp__sub_82D7CCE0(ctx, base);
#else
  __imp__sub_82CBB1F8(ctx, base);
#endif
  FlushPlane(caller, address, length);
}
