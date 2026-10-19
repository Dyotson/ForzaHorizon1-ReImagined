// tu_address_map: TU4 addresses
// #182: eviction must not destroy the cache's last reference between a reader's
// pointer load and virtual AddRef. Lookup already holds this cache's reader
// lock, but the original sole-owner eviction doesn't take its writer lock.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include "resource_cache_lock.h"

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>
#include <atomic>
#include <cstdlib>

#if FORZA_TU4
extern "C" REX_FUNC(__imp__RtlTryEnterCriticalSection);
extern "C" REX_FUNC(__imp__RtlLeaveCriticalSection);
extern "C" REX_FUNC(sub_831D0148);
extern "C" REX_FUNC(__imp__sub_82D25DD8);

namespace {
class TryCacheWriter {
 public:
  TryCacheWriter(const PPCContext& caller, uint8_t* base, uint32_t cache)
      : context_(caller), base_(base), lock_(cache + 112) {
    // The trim caller owns the global eviction-list critical section. Waiting
    // for cache readers here would invert lookup -> eviction-list lock order.
    // Try the writer critical section, then claim only an idle reader counter.
    context_.r3.u64 = lock_ + 8;
    __imp__RtlTryEnterCriticalSection(context_, base_);
    if (!context_.r3.u32) return;
    auto& counter = *reinterpret_cast<uint32_t*>(forza::GuestPtr(base_, lock_));
    owned_ = forza::resource::TryClaimIdleCache(counter);
    if (!owned_) {
      context_.r3.u64 = lock_ + 8;
      __imp__RtlLeaveCriticalSection(context_, base_);
    }
  }
  ~TryCacheWriter() {
    if (owned_) {
      context_.r3.u64 = lock_;
      // Preserve the guest's reader-wakeup and writer-owner release protocol.
      sub_831D0148(context_, base_);
    }
  }
  explicit operator bool() const { return owned_; }
  TryCacheWriter(const TryCacheWriter&) = delete;
  TryCacheWriter& operator=(const TryCacheWriter&) = delete;

 private:
  PPCContext context_;
  uint8_t* base_;
  uint32_t lock_;
  bool owned_ = false;
};
}  // namespace

extern "C" REX_FUNC(sub_82D25DD8) {
  static const bool trace = [] {
    const char* value = std::getenv("FORZA_RESOURCE_LIFETIME_TRACE");
    return value && *value && *value != '0';
  }();
  static std::atomic<unsigned> reports{0};
  const uint32_t cache = ctx.r3.u32;
  const uint32_t entry = ctx.r4.u32;
  TryCacheWriter writer(ctx, base, ctx.r3.u32);
  if (!writer) {
    if (trace && reports.fetch_add(1) < 20)
      REXLOG_INFO("RESOURCE CACHE: TU4 eviction deferred for busy cache {:08X}, entry {} (#182)", cache, entry);
    // Return zero bytes reclaimed; the existing trimming loop can try another
    // entry/cache. Never drop a live reference or block while holding its list.
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82D25DD8(ctx, base);
  if (trace && reports.fetch_add(1) < 20)
    REXLOG_INFO("RESOURCE CACHE: TU4 eviction guarded cache {:08X}, entry {}, reclaimed {} bytes (#182)", cache, entry, ctx.r3.u32);
}
#endif
