// Opt-in local denial experiment at TU4's native per-player car eligibility check.
// FORZA_UNICORN_ELIGIBILITY_TEST=deny / grant; unset leaves the native result alone.
// Not an authenticated server policy. See docs/unicorn_online_rewards.md.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include "../platform/unicorn_eligibility.h"
#include "../kernel/live.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>

// The traced function and player layout have only been established for TU4.
#if FORZA_TU4
extern "C" REX_FUNC(__imp__sub_829457A0);
extern "C" REX_FUNC(__imp__sub_828653D8);
extern "C" REX_FUNC(__imp__sub_827931E8);
extern "C" REX_FUNC(__imp__sub_82675B00);

namespace {
uint32_t Read32(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = forza::GuestPtr(base, address);
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (24 - 8 * i));
}

uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<uint64_t> denial_until{0};
thread_local uint32_t event_dialog = 0;

// Same native reference-counted UTF-16 text construction used by graphics_menu.
// The parameter object retains its own reference; neither scratch nor the caller's
// reference survives this synchronous assignment.
void SetText(PPCContext ctx, uint8_t* base, uint32_t target,
             std::string_view text) {
  if (text.size() > 240) return;
  const uint32_t scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(1024);
  if (!scratch) return;
  auto call = [&](uint32_t function, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    ctx.r3.u64 = a; ctx.r4.u64 = b; ctx.r5.u64 = c;
    rex::runtime::ResolveIndirectFunction(function)(ctx, base);
    return ctx.r3.u32;
  };
  auto* buffer = forza::GuestPtr(base, scratch + 32);
  for (size_t i = 0; i < text.size(); ++i) {
    buffer[i * 2] = 0;
    buffer[i * 2 + 1] = uint8_t(text[i]);
  }
  buffer[text.size() * 2] = buffer[text.size() * 2 + 1] = 0;
  const uint32_t object = call(0x824265C8, 16);
  if (object) {
    Write32(base, object, 0x82001674);
    Write32(base, object + 4, 0);
    Write32(base, object + 8, 0);
    Write32(base, object + 12, 0);
    call(0x82D6DE20, object, scratch + 32, uint32_t(text.size()));
    call(0x82555B90, scratch, object);
    call(0x82471820, target, scratch);
    call(0x824CEAB0, scratch);
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(scratch);
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
  }
  if (denied && native_result) denial_until.store(NowMs() + 5000);
}

// TU4 popup 19 is RuleChangeInvalidatedCar; 24 is NoCarsAvailable, used when
// free roam cannot allocate a fallback. Scope the text substitution
// to this constructor, never to all localized copies of its title/body strings.
extern "C" REX_FUNC(sub_827931E8) {
  const uint32_t previous = event_dialog;
  const uint32_t popup = ctx.r4.u32;
  const uint64_t until = denial_until.load();
  const uint32_t game = Read32(base, FORZA_ADDR(0x83305CC4));
  event_dialog = (popup == 19 || popup == 24) && Mode() == forza::unicorn::TestMode::Deny &&
                 forza::NetworkOn() && game && Read32(base, game + 0x28) &&
                 until && NowMs() <= until ? popup : 0;
  if (event_dialog) denial_until.store(0);
  __imp__sub_827931E8(ctx, base);
  event_dialog = previous;
}

extern "C" REX_FUNC(sub_82675B00) {
  if (event_dialog && ctx.r4.u32 == 0xF1 && ctx.r5.u32) {
    const uint32_t parameters = Read32(base, ctx.r5.u32);
    if (parameters) {
      SetText(ctx, base, parameters + 0xC, "EVENT UNLOCK REQUIRED - LOCAL TEST");
      SetText(ctx, base, parameters + 0x10,
          event_dialog == 19 ?
              "This unicorn needs an event reward unlock for multiplayer. "
              "A rental car has been assigned. Use Select Car to choose another car. "
              "You can still use your unicorn in single player." :
              "This unicorn needs an event reward unlock for online free roam. "
              "Use Select Car to choose another car before joining. "
              "You can still use your unicorn in single player.");
      REXLOG_INFO("UNICORN TEST: native event-unlock dialog substituted (popup {})", event_dialog);
    }
  }
  __imp__sub_82675B00(ctx, base);
}
#endif
