// TU4 borrowed cars can retain the shared undamaged texture across a race load.
// If its first request inherits the compressed GPU pool, that one live texture
// prevents the pool from closing and the loader waits forever in state 22.
// Give this shared fallback the fixed GPU pool before its first load is queued.
// Its reference counting, unloading and allocation failure handling stay native.
#include "../platform/guest_memory.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstring>

// These are explicitly traced TU4 addresses, not disc addresses to remap.
#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4
extern "C" REX_FUNC(__imp__sub_82F0BF68);
extern "C" REX_FUNC(__imp__RtlEnterCriticalSection);
extern "C" REX_FUNC(__imp__RtlLeaveCriticalSection);

namespace {
uint32_t Read32(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool SharedDamageMask(uint8_t* base, uint32_t resource) {
  constexpr char name[] = "game:\\media\\cars\\nul_car_00\\nodamage.xds";
  if (Read32(base, resource) != 0x8200224C ||
      Read32(base, resource + 0x64) != sizeof(name) - 1) return false;
  const uint32_t text = Read32(base, resource + 0x68) >= 16
                            ? Read32(base, resource + 0x54) : resource + 0x54;
  return std::memcmp(forza::GuestPtr(base, text), name, sizeof(name) - 1) == 0;
}
}  // namespace

extern "C" REX_FUNC(sub_82F0BF68) {
  const uint32_t resource = ctx.r4.u32;
  if (resource && SharedDamageMask(base, resource)) {
    PPCContext lock = ctx;
    lock.r3.u64 = resource + 4;
    __imp__RtlEnterCriticalSection(lock, base);
    // 9 means inherit the requesting thread's pool. Never retarget an existing
    // allocation: the original queue function captures the pool under this lock.
    if (Read32(base, resource + 0x2C) == 9) {
      auto* pool = forza::GuestPtr(base, resource + 0x2C);
      pool[0] = pool[1] = pool[2] = 0;
      pool[3] = 0x21;  // fixed (1), GPU (0x20)
    }
    lock = ctx;
    lock.r3.u64 = resource + 4;
    __imp__RtlLeaveCriticalSection(lock, base);
  }
  __imp__sub_82F0BF68(ctx, base);
}
#endif
