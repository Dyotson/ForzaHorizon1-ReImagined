// Server event rewards at TU4's native per-player car eligibility check.
// Optional local deny/grant experiment remains available for disposable tests.
// Client-side cooperation, not authoritative remote-car enforcement.
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
extern "C" REX_FUNC(__imp__sub_8279D608);
extern "C" REX_FUNC(__imp__sub_827B2048);
extern "C" REX_FUNC(__imp__sub_827C73F0);
extern "C" REX_FUNC(sub_827931E8);

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
std::atomic<forza::EventPermission> denial_reason{forza::EventPermission::Denied};
std::atomic<bool> denial_test{false};
// A rejected World choice may outlive the countdown: the other players can
// launch while this player remains in the lobby. Retain the precise rejected
// choice so Jump In can reopen the picker instead of looping No Cars Available.
std::atomic<uint32_t> rejected_game{0}, rejected_player{0}, rejected_model{0};
thread_local uint32_t event_dialog = 0;
thread_local forza::EventPermission dialog_reason = forza::EventPermission::Denied;
thread_local bool dialog_test = false;
thread_local bool workshop_dialog = false;

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

// The in-world garage accepts a database row directly, bypassing the lobby's
// per-player eligibility check. Check CarId before the native callback updates
// the player's choices or closes the picker. Leave the old car and save intact.
extern "C" REX_FUNC(sub_827C73F0) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t row = ctx.r4.u32;
  const uint32_t game = Read32(base, FORZA_ADDR(0x83305CC4));
  if (screen && row && forza::NetworkOn() && game &&
      Read32(base, game + 0x28) && Read32(base, game + 0x30) == 1) {
    PPCContext lookup = ctx;
    lookup.r3.u64 = row;
    lookup.r4.u64 = 0x82007E8C;  // Native database field name: CarId.
    rex::runtime::ResolveIndirectFunction(Read32(base, Read32(base, row) + 0x14))(lookup, base);
    const uint32_t model = lookup.r3.u32;
    const auto permission = forza::LiveEventPermission(model);
    const bool server_denied = permission != forza::EventPermission::Unrestricted &&
                               permission != forza::EventPermission::Allowed;
    const bool test_denied = forza::unicorn::TestDenies(
        Mode(), true, Read32(base, game + 0x28), true, model);
    REXLOG_INFO("UNICORN REWARD: workshop model {} denied {} permission {}",
                model, server_denied || test_denied, int(permission));
    if (server_denied || test_denied) {
      denial_reason.store(server_denied ? permission : forza::EventPermission::Denied);
      denial_test.store(!server_denied && test_denied);
      denial_until.store(NowMs() + 5000);
      const bool previous = workshop_dialog;
      workshop_dialog = true;
      lookup = ctx;
      lookup.r3.u64 = screen;
      lookup.r4.u64 = 24;  // Native OK dialog; retain the car picker underneath.
      lookup.r5.u64 = screen;
      sub_827931E8(lookup, base);
      workshop_dialog = previous;
      return;
    }
  }
  __imp__sub_827C73F0(ctx, base);
}

extern "C" REX_FUNC(sub_829457A0) {
  const uint32_t player = ctx.r4.u32;
  __imp__sub_829457A0(ctx, base);
  const auto mode = Mode();
  if (!player || !forza::NetworkOn()) return;

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
  rejected_game.store(0);
  rejected_player.store(0);
  rejected_model.store(0);
  const bool native_result = ctx.r3.u8 != 0;
  const auto permission = forza::LiveEventPermission(model);
  if (mode == forza::unicorn::TestMode::Off && permission == forza::EventPermission::Unrestricted) return;
  const bool server_denied = permission != forza::EventPermission::Unrestricted &&
                             permission != forza::EventPermission::Allowed;
  const bool test_denied = forza::unicorn::TestDenies(mode, true, online_mode, local, model);
  const bool denied = server_denied || test_denied;
  ctx.r3.u64 = native_result && !denied;

  // This check may run repeatedly. Show/log a changed decision once, not every frame.
  static std::atomic<uint64_t> previous{~uint64_t{0}};
  const uint64_t decision = uint64_t(model) << 3 | uint64_t(denied) << 2 |
                            uint64_t(native_result) << 1 | uint64_t(ctx.r3.u8 != 0);
  if (previous.exchange(decision) != decision) {
    REXLOG_INFO("UNICORN {}: model {} mode {} native {} denied {} eligible {} permission {}",
                mode == forza::unicorn::TestMode::Off ? "REWARD" : "TEST",
                model, online_mode, native_result, denied, ctx.r3.u8 != 0, int(permission));
  }
  if (denied && native_result) {
    rejected_game = game;
    rejected_player = player;
    rejected_model = model;
    denial_reason.store(server_denied ? permission : forza::EventPermission::Denied);
    denial_test.store(!server_denied && test_denied);
    denial_until.store(NowMs() + 5000);
  }
}

// The two native launch callbacks call this preflight before starting/loading.
// Only a previously policy-rejected local World choice gets this recovery.
// Calling the normal World picker preserves its callbacks and eligibility check.
extern "C" REX_FUNC(sub_8279D608) {
  const uint32_t caller = uint32_t(ctx.lr);
  if ((caller == 0x8279D710 || caller == 0x8279D7D0) &&
      rejected_game && forza::NetworkOn()) {
    const uint32_t game = Read32(base, FORZA_ADDR(0x83305CC4));
    if (game == rejected_game && Read32(base, ctx.r3.u32 + 0x174) == game &&
        Read32(base, game + 0x28) == 7 && Read32(base, game + 0x30) == 1) {
      const PPCContext saved = ctx;
      ctx.r3.u64 = game;
      __imp__sub_828653D8(ctx, base);
      const uint32_t player = ctx.r3.u32;
      ctx = saved;
      if (player == rejected_player) {
        const uint32_t status = Read32(base, player + 0xD0);
        const uint32_t descriptor = Read32(base, player + 0xD4);
        if (status && descriptor && forza::GuestPtr(base, status)[6] == 2 &&
            Read32(base, descriptor + 8) == rejected_model) {
          REXLOG_INFO("UNICORN REWARD: Jump In reopening rejected World car selection");
          __imp__sub_827B2048(ctx, base);
          ctx.r3.u64 = 1;  // Stop the launch callback while the picker is open.
          return;
        }
      }
    }
  }
  __imp__sub_8279D608(ctx, base);
}

// TU4 popup 19 is RuleChangeInvalidatedCar; 24 is NoCarsAvailable, used when
// free roam cannot allocate a fallback. Scope the text substitution
// to this constructor, never to all localized copies of its title/body strings.
extern "C" REX_FUNC(sub_827931E8) {
  const uint32_t previous = event_dialog;
  const uint32_t popup = ctx.r4.u32;
  const uint64_t until = denial_until.load();
  const uint32_t game = Read32(base, FORZA_ADDR(0x83305CC4));
  event_dialog = (popup == 19 || popup == 24) &&
                 forza::NetworkOn() && game && Read32(base, game + 0x28) &&
                 until && NowMs() <= until ? popup : 0;
  const auto previous_reason = dialog_reason;
  const bool previous_test = dialog_test;
  if (event_dialog) {
    denial_until.store(0);
    dialog_reason = denial_reason.load();
    dialog_test = denial_test.load();
  }
  __imp__sub_827931E8(ctx, base);
  event_dialog = previous;
  dialog_reason = previous_reason;
  dialog_test = previous_test;
}

extern "C" REX_FUNC(sub_82675B00) {
  if (event_dialog && ctx.r4.u32 == 0xF1 && ctx.r5.u32) {
    const uint32_t parameters = Read32(base, ctx.r5.u32);
    if (parameters) {
      const char* title = dialog_test ? "EVENT UNLOCK REQUIRED - LOCAL TEST" : "EVENT UNLOCK REQUIRED";
      const char* message = event_dialog == 19 ?
              "This unicorn needs an event reward unlock for multiplayer. "
              "A rental car has been assigned. Use Select Car to choose another car. "
              "You can still use your unicorn in single player." :
              "This unicorn needs an event reward unlock for multiplayer. "
              "Use Select Car to choose another car before joining. "
              "You can still use your unicorn in single player.";
      if (workshop_dialog) {
        message = "This unicorn needs an event reward unlock for multiplayer. "
                  "Choose another car, or go back to keep your current car. "
                  "You can still use your unicorn in single player.";
      }
      if (dialog_reason == forza::EventPermission::Pending) {
        title = "CHECKING EVENT UNLOCKS";
        message = "Your event unlocks are being checked. Try Select Car again shortly, or choose another car. "
                  "You can still use your unicorn in single player.";
      } else if (dialog_reason == forza::EventPermission::Unavailable) {
        title = "COULDN'T VERIFY EVENT UNLOCK";
        message = "The server's event unlock could not be verified. Retry Select Car, choose another car, "
                  "or return to single player. Your garage has not been changed.";
      } else if (dialog_reason == forza::EventPermission::EnrollmentRequired) {
        title = "EVENT REGISTRATION REQUIRED";
        message = "Ask the server organizer to register your player identity for event rewards. "
                  "Choose another car for multiplayer. You can still use your unicorn in single player.";
      }
      SetText(ctx, base, parameters + 0xC, title);
      SetText(ctx, base, parameters + 0x10, message);
      REXLOG_INFO("UNICORN {}: native event-unlock dialog substituted (popup {})",
                  dialog_test ? "TEST" : "REWARD", event_dialog);
    }
  }
  __imp__sub_82675B00(ctx, base);
}
#endif
