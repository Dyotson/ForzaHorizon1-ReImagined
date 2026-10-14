// forza - FORZA_DLC_LOG=1 logs the game's answers about DLC cars (docs/dlc.md).
//
// The game's database has SQL functions for content (registered in sub_8250B9xx):
// IsCarInstalled (sub_82502DD8), IsCarInstalledAndPurchased (sub_82502E18) and IsCarOnDisc
// (sub_824F1A80). Their argument is the car id (r3) and a content type (r4). Each
// (function, id, type) is logged once, with the answer, so opening the Autoshow lists which
// cars the game counts as installed and owned.

#include "../platform/guest_addr.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <tuple>

extern "C" REX_FUNC(__imp__sub_82502DD8);
extern "C" REX_FUNC(__imp__sub_82502E18);
extern "C" REX_FUNC(__imp__sub_824F1A80);

namespace {

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_DLC_LOG");
    return v && *v && *v != '0';
  }();
  return on;
}

void Log(const char* fn, uint32_t id, uint32_t type, uint32_t result) {
  static std::mutex mutex;
  static std::set<std::tuple<const char*, uint32_t, uint32_t>> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (seen.emplace(fn, id, type).second) {
    REXLOG_INFO("DLC: {}(id {}, type {}) = {}", fn, id, type, result & 0xFF);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_82502DD8) {
  const uint32_t id = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_82502DD8(ctx, base);
  if (Enabled()) Log("IsCarInstalled", id, type, ctx.r3.u32);
}

extern "C" REX_FUNC(sub_82502E18) {
  const uint32_t id = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_82502E18(ctx, base);
  if (Enabled()) Log("IsCarInstalledAndPurchased", id, type, ctx.r3.u32);
}

extern "C" REX_FUNC(sub_824F1A80) {
  const uint32_t id = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_824F1A80(ctx, base);
  if (Enabled()) Log("IsCarOnDisc", id, type, ctx.r3.u32);
}
