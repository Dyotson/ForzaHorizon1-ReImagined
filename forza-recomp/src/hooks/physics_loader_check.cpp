// forza - diagnostics for the free-roam physics-loader fault (docs/PROJECT.md).
//
// sub_82D3DB00(object, index, ...) takes the 12-byte record object->+84[index] and copies
// one 544-byte entry per item into an 8-slot array in its stack frame; the item count is
// the record's byte 9. In the failing runs the record is garbage (count above 8, list
// pointer 0xF) and the loop runs past the frame. This hook (hints/physics_loader_check.toml)
// sits after the count is loaded and logs the record, the table and the object the first
// times the count is out of range, so a later run can watch who writes them.
// FORZA_PHYS_SEED=1 also fills the never-written w lanes of the GJK simplex and EPA vertex
// slots with sentinels naming the slot (the fault's cause was such a slot: the GJK stopped
// before adding a vertex because a dot product overflowed to infinity instead of Xenon's
// QNaN; SDK patch 0025 fixed that in the codegen), so reading one is caught every time.

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

static_assert(offsetof(PPCContext, r3) == 0, "hooks recover ctx from &ctx.r3");

namespace {

uint32_t Load32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n; ++i) s += fmt::format("{}{:02X}", i && !(i % 4) ? " " : "", p[i]);
  return s;
}

std::atomic<int> reports{0};

}  // namespace

void ForzaCheckPhysicsRecord_82D3DB54(PPCRegister& r3) {
  PPCContext& ctx = *reinterpret_cast<PPCContext*>(&r3);
  const uint32_t count = ctx.r8.u32 & 0xFF;
  if (count <= 8) return;
  if (reports.fetch_add(1) >= 5) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t object = ctx.r31.u32, record = ctx.r29.u32, index = ctx.r26.u32;
  const uint32_t table = Load32(forza::GuestPtr(base, object + 84));
  REXLOG_ERROR("PHYSCHECK: sub_82D3DB00 record count {} > 8: object {:08X} index {} table "
               "{:08X} record {:08X} lr {:08X}",
               count, object, index, table, record, uint32_t(ctx.lr));
  REXLOG_ERROR("PHYSCHECK: record-24..+36: {}", Hex(forza::GuestPtr(base, record - 24), 60));
  REXLOG_ERROR("PHYSCHECK: object+0..+128: {}", Hex(forza::GuestPtr(base, object), 128));
}

namespace {
constexpr uint32_t kSentinel = 0xEEEEEEC0;  // low byte 0xC0, the index seen in the faults
std::atomic<uint64_t> seeded{0}, unwritten{0}, bad{0};

void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
}  // namespace

void ForzaSeedPhysicsIndices_82D44E4C(PPCRegister& r3) {
  PPCContext& ctx = *reinterpret_cast<PPCContext*>(&r3);
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  Store32(forza::GuestPtr(base, ctx.r1.u32 + 396), kSentinel);
  Store32(forza::GuestPtr(base, ctx.r1.u32 + 412), kSentinel);
  ++seeded;
}

void ForzaCheckPhysicsIndices_82D44FE4(PPCRegister& r3) {
  PPCContext& ctx = *reinterpret_cast<PPCContext*>(&r3);
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t a = Load32(forza::GuestPtr(base, ctx.r1.u32 + 396));
  const uint32_t b = Load32(forza::GuestPtr(base, ctx.r1.u32 + 412));
  const bool a_unwritten = a == kSentinel, b_unwritten = b == kSentinel;
  if (a_unwritten || b_unwritten) {
    const uint64_t n = ++unwritten;
    if (n <= 5 || n % 1000 == 0) {
      REXLOG_ERROR("PHYSCHECK: index words not written by the solver ({} of {} uses): +396 {:08X} "
                   "+412 {:08X}, path byte r24={}",
                   n, seeded.load(), a, b, ctx.r24.u32 & 0xFF);
    }
  } else if ((a & 0xFF) >= 64 || (b & 0xFF) >= 64) {
    const uint64_t n = ++bad;
    if (n <= 5) {
      REXLOG_ERROR("PHYSCHECK: solver wrote large indices: +396 {:08X} +412 {:08X}", a, b);
    }
  }
}

void ForzaSeedEpaSlots_82D43E40(PPCRegister& r3) {
  static const bool on = std::getenv("FORZA_PHYS_SEED") != nullptr;
  if (!on) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t ws = r3.u32;
  for (uint32_t slot = 0; slot < 30; ++slot) {
    const uint32_t rec = ws + 0x1AA0 + slot * 32;
    Store32(forza::GuestPtr(base, rec + 12), 0xEEEE00C0u | slot << 8);
    Store32(forza::GuestPtr(base, rec + 28), 0xDDDD00C0u | slot << 8);
  }
}

void ForzaSeedSimplex_82D12178(PPCRegister& r3) {
  static const bool on = std::getenv("FORZA_PHYS_SEED") != nullptr;
  if (!on) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  for (uint32_t pos = 0; pos < 4; ++pos) {
    const uint32_t v = r3.u32 + 16 + pos * 32;
    Store32(forza::GuestPtr(base, v + 12), 0xCCCC00C0u | pos << 8);
    Store32(forza::GuestPtr(base, v + 28), 0xBBBB00C0u | pos << 8);
  }
}
