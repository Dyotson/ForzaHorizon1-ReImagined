// tu_address_map: TU4 addresses
// #182: serialize a cache pointer's load/retain with sole-owner eviction.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include "resource_cache_lock.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>
#include <atomic>
#include <bit>
#include <cstdlib>

#if FORZA_TU4
extern "C" REX_FUNC(__imp__sub_82471820);
extern "C" REX_FUNC(__imp__sub_82D25DD8);

namespace {
forza::resource::CachePinGate pin_gate;

uint32_t Read32(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t GuestWord(uint32_t value) {
  if constexpr (std::endian::native == std::endian::little) return __builtin_bswap32(value);
  return value;
}
void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<uint32_t*>(forza::GuestPtr(base, address)) = GuestWord(value);
}
void Reference(const PPCContext& caller, uint8_t* base, uint32_t object, uint32_t slot) {
  PPCContext call = caller;
  // Reserve an aligned guest call frame, preserving the caller's registers
  // and backchain while the original virtual reference method executes.
  call.r1.u64 = caller.r1.u32 - 128;
  Write32(base, call.r1.u32, caller.r1.u32);
  Write32(base, caller.r1.u32 - 8, uint32_t(caller.lr));
  call.r3.u64 = object;
  call.lr = slot == 8 ? 0x82471858 : 0x82471878;
  const auto target = Read32(base, Read32(base, object) + slot);
  call.ctr.u64 = target;
  call.last_indirect_target = target;
  rex::runtime::ResolveIndirectFunction(target)(call, base);
}
}  // namespace

extern "C" REX_FUNC(sub_82471820) {
  if (!forza::resource::IsCacheCopyReturn(uint32_t(ctx.lr))) {
    __imp__sub_82471820(ctx, base);
    return;
  }
  const uint32_t destination = ctx.r3.u32, source = ctx.r4.u32;
  pin_gate.Copy(
      [&] { return Read32(base, source); },
      [&](uint32_t object) { Reference(ctx, base, object, 8); },
      [&](uint32_t incoming) {
        // Match the load/unload atomic take at resource+0x40: publication
        // must not release a destination another thread already took.
        auto& word = *reinterpret_cast<uint32_t*>(forza::GuestPtr(base, destination));
        return GuestWord(std::atomic_ref<uint32_t>(word).exchange(GuestWord(incoming)));
      },
      [&](uint32_t object) { Reference(ctx, base, object, 12); });
  ctx.r3.u64 = destination;
}

extern "C" REX_FUNC(sub_82D25DD8) {
  static const bool trace = [] {
    const char* value = std::getenv("FORZA_RESOURCE_LIFETIME_TRACE");
    return value && *value && *value != '0';
  }();
  static std::atomic<uint64_t> busy_reports{0}, guarded_reports{0};
  const uint32_t cache = ctx.r3.u32, entry = ctx.r4.u32;
  auto eviction = pin_gate.TryEvict();
  if (!eviction) {
    if (trace && busy_reports.fetch_add(1) < 20)
      REXLOG_INFO("RESOURCE CACHE: TU4 eviction deferred for active pin, cache {:08X}, entry {} (#182)", cache, entry);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82D25DD8(ctx, base);
  if (trace && guarded_reports.fetch_add(1) < 20)
    REXLOG_INFO("RESOURCE CACHE: TU4 eviction guarded cache {:08X}, entry {}, reclaimed {} bytes (#182)", cache, entry, ctx.r3.u32);
}
#endif
