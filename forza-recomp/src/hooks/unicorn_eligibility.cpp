// Opt-in local denial experiment at TU4's native per-player car eligibility check.
// FORZA_UNICORN_ELIGIBILITY_TEST=deny / grant; unset leaves the native result alone.
// Not an authenticated server policy. See docs/unicorn_online_rewards.md.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include "../platform/hud_overlay.h"
#include "../platform/unicorn_eligibility.h"
#include "../kernel/live.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <atomic>
#include <cstdlib>
#include <string_view>

// The traced function and player layout have only been established for TU4.
#if FORZA_TU4
extern "C" REX_FUNC(__imp__sub_829457A0);
extern "C" REX_FUNC(__imp__sub_828653D8);

namespace {
uint32_t Read32(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

forza::unicorn::TestMode Mode() {
  static const auto mode = [] {
    const char* value = std::getenv("FORZA_UNICORN_ELIGIBILITY_TEST");
    const std::string_view text = value ? value : "";
    if (text == "deny") return forza::unicorn::TestMode::Deny;
    if (text == "grant") return forza::unicorn::TestMode::Granted;
    return forza::unicorn::TestMode::Off;
  }();
  return mode;
}
}  // namespace

extern "C" REX_FUNC(sub_829457A0) {
  const uint32_t player = ctx.r4.u32;
  __imp__sub_829457A0(ctx, base);
  const auto mode = Mode();
  if (mode == forza::unicorn::TestMode::Off || !player || !forza::NetworkOn()) return;

  const uint32_t game = Read32(base, FORZA_ADDR(0x83305CC4));
  if (!game) return;
  const uint32_t online_mode = Read32(base, game + 0x28);
  if (!online_mode) return;

  // Restrict only the actual local player, not AI or another console's records.
  // Preserve all outputs from the native eligibility check around the getter call.
  const PPCContext saved = ctx;
  ctx.r3.u64 = game;
  __imp__sub_828653D8(ctx, base);
  const bool local = ctx.r3.u32 == player;
  ctx = saved;
  if (!local) return;

  const uint32_t descriptor = Read32(base, player + 0xD4);
  if (!descriptor) return;
  const uint32_t model = Read32(base, descriptor + 8);
  const bool native_result = ctx.r3.u8 != 0;
  const bool denied = forza::unicorn::TestDenies(mode, true, online_mode, local, model);
  ctx.r3.u64 = forza::unicorn::Eligible(native_result, mode, true, online_mode, local, model);

  // This check may run repeatedly. Show/log a changed decision once, not every frame.
  static std::atomic<uint64_t> previous{~uint64_t{0}};
  const uint64_t decision = uint64_t(model) << 3 | uint64_t(denied) << 2 |
                            uint64_t(native_result) << 1 | uint64_t(ctx.r3.u8 != 0);
  if (previous.exchange(decision) != decision) {
    REXLOG_INFO("UNICORN TEST: model {} mode {} native {} denied {} eligible {} (local experiment)",
                model, online_mode, native_result, denied, ctx.r3.u8 != 0);
    if (denied) {
      forza::HudShow("Event unlock required - local test",
                     "This unicorn needs an event unlock for multiplayer.\n"
                     "Choose another car in Select Car. It remains available in single player.", 8);
    }
  }
}
#endif
