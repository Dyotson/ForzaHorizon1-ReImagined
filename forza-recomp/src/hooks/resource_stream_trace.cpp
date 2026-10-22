// Opt-in evidence for the streamed collision-block failure in #182/#232.
// These TU4 functions do not use the cache-copy call sites guarded by #184.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#if FORZA_TU4
extern "C" REX_FUNC(__imp__sub_82D39EA0);
extern "C" REX_FUNC(__imp__sub_82DDA2A0);
extern "C" REX_FUNC(__imp__sub_82DD8150);
extern "C" REX_FUNC(__imp__sub_82DD20C8);
extern "C" REX_FUNC(__imp__sub_82E346A0);

namespace {
bool Trace() {
  static const bool enabled = [] {
    const char* p = std::getenv("FORZA_RESOURCE_STREAM_TRACE");
    return p && *p && *p != '0';
  }();
  return enabled;
}
bool Readable(uint32_t address, uint32_t size) {
  if (!size || uint64_t(address) + size > 0x100000000ull) return false;
  auto* heap = rex::system::kernel_state()->memory()->LookupHeap(address);
  uint32_t protect = 0;
  return heap && heap->QueryProtect(address, &protect) &&
         (protect & rex::memory::kMemoryProtectRead) &&
         heap->QueryProtect(address + size - 1, &protect) &&
         (protect & rex::memory::kMemoryProtectRead);
}
uint32_t Read(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
using Header = std::array<uint32_t, 16>;
Header Snapshot(uint8_t* base, uint32_t address) {
  Header result{};
  for (uint32_t i = 0; i < result.size(); ++i) result[i] = Read(base, address + 4 * i);
  return result;
}
struct Pending {
  uint32_t reference, index;
  Header header;
  bool consumed = false;
};
std::mutex pending_mutex;
std::unordered_map<uint32_t, Pending> pending;
std::atomic<uint64_t> reads{0}, relocations{0};
bool Sample(uint64_t n) { return n < 16 || n % 256 == 0; }
}  // namespace

extern "C" REX_FUNC(sub_82D39EA0) {
  if (!Trace() || uint32_t(ctx.lr) != 0x82E364A4) {
    __imp__sub_82D39EA0(ctx, base);
    return;
  }
  const uint32_t stream = ctx.r3.u32, buffer = ctx.r4.u32, wanted = ctx.r5.u32;
  __imp__sub_82D39EA0(ctx, base);
  const auto n = reads.fetch_add(1);
  if (ctx.r3.u32 != wanted)
    REXLOG_WARN("RESOURCE STREAM: short read stream {:08X} buffer {:08X}: {} of {} bytes (#232)",
                stream, buffer, ctx.r3.u32, wanted);
  else if (Sample(n))
    REXLOG_INFO("RESOURCE STREAM: full read stream {:08X} buffer {:08X}: {} bytes (#232)",
                stream, buffer, wanted);
}

extern "C" REX_FUNC(sub_82DDA2A0) {
  if (Trace() && Readable(ctx.r3.u32, 64) && Readable(ctx.r5.u32, 4)) {
    const uint32_t reference = Read(base, ctx.r5.u32);
    std::lock_guard lock(pending_mutex);
    auto previous = pending.find(ctx.r3.u32);
    if (previous != pending.end() && !previous->second.consumed &&
        previous->second.reference != reference)
      REXLOG_WARN("RESOURCE STREAM: queued block {:08X} reused by owner {:08X} while owner "
                  "{:08X} is still queued (#232)", ctx.r3.u32, reference, previous->second.reference);
    // A diagnostic must not grow without bound if a manager stops consuming.
    if (pending.size() < 4096 || pending.contains(ctx.r3.u32))
      pending.insert_or_assign(ctx.r3.u32, Pending{reference, ctx.r4.u32, Snapshot(base, ctx.r3.u32)});
  }
  __imp__sub_82DDA2A0(ctx, base);
}

extern "C" REX_FUNC(sub_82DD8150) {
  const bool consuming = Trace() && uint32_t(ctx.lr) == 0x82DDA0A8;
  const uint32_t destination = ctx.r3.u32;
  __imp__sub_82DD8150(ctx, base);
  if (consuming && Readable(destination, 16)) {
    std::lock_guard lock(pending_mutex);
    auto it = pending.find(Read(base, destination));
    if (it != pending.end() && it->second.reference == Read(base, destination + 4))
      it->second.consumed = true;
  }
}

extern "C" REX_FUNC(sub_82DD20C8) {
  if (Trace() && uint32_t(ctx.lr) == 0x82DDA118 && Readable(ctx.r3.u32, 64)) {
    const uint32_t block = ctx.r3.u32;
    const Header header = Snapshot(base, block);
    Header published{};
    bool queued = false, changed = false;
    uint32_t reference = 0, index = 0;
    {
      std::lock_guard lock(pending_mutex);
      auto it = pending.find(block);
      if (it != pending.end()) {
        queued = true;
        changed = header != it->second.header;
        published = it->second.header;
        reference = it->second.reference;
        index = it->second.index;
        pending.erase(it);
      }
    }
    if (changed)
      for (uint32_t i = 0; i < header.size(); ++i)
        if (header[i] != published[i])
          REXLOG_WARN("RESOURCE STREAM: block {:08X} +{:X} changed {:08X} -> {:08X} "
                      "between queue and relocation (#232)", block, i * 4, published[i], header[i]);
    const auto n = relocations.fetch_add(1);
    if (changed || !queued || Sample(n))
      REXLOG_INFO("RESOURCE STREAM: relocate block {:08X} bytes {} owner {:08X} slot {:08X} "
                  "queued {} changed {} ref {:08X} count {} index {} "
                  "header {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} (#232)",
                  block, ctx.r5.u32, ctx.r30.u32, ctx.r31.u32, queued, changed, reference,
                  Readable(reference, 8) ? Read(base, reference + 4) : 0, index,
                  header[0], header[1], header[2], header[3], header[4], header[5], header[14]);
  }
  __imp__sub_82DD20C8(ctx, base);
}

extern "C" REX_FUNC(sub_82E346A0) {
  if (Trace()) {
    std::lock_guard lock(pending_mutex);
    for (auto it = pending.begin(); it != pending.end();) {
      if (it->second.reference == ctx.r3.u32) {
        if (!it->second.consumed)
          REXLOG_WARN("RESOURCE STREAM: owner {:08X} destroyed before queued block {:08X} "
                      "index {} was consumed (#232)", ctx.r3.u32, it->first, it->second.index);
        it = pending.erase(it);
      } else ++it;
    }
  }
  __imp__sub_82E346A0(ctx, base);
}
#endif
