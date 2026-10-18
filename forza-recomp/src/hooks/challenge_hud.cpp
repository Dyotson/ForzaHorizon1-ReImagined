// The game's CHudCombinedNotificationControl: headline and caption, using
// native bindings and SHOW/HIDE animations. Never register an ImGui dialog.
// tu_address_map: TU4 addresses
#include "../platform/challenge_hud.h"
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <chrono>
#include <cstring>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if FORZA_TU4
#define HudUpdate sub_828A0608
#define OriginalUpdate __imp__sub_828A0608
#define HudBind sub_8289AF88
#define OriginalBind __imp__sub_8289AF88
#define HudUnbind sub_8289B020
#define OriginalUnbind __imp__sub_8289B020
#define HudNotification sub_828A2980
#define OriginalNotification __imp__sub_828A2980
#else
#define HudUpdate sub_827ED4D8
#define OriginalUpdate __imp__sub_827ED4D8
#define HudBind sub_827E8000
#define OriginalBind __imp__sub_827E8000
#define HudUnbind sub_827E8098
#define OriginalUnbind __imp__sub_827E8098
#define HudNotification sub_827EF050
#define OriginalNotification __imp__sub_827EF050
#endif
extern "C" REX_FUNC(OriginalUpdate);
extern "C" REX_FUNC(OriginalBind);
extern "C" REX_FUNC(OriginalUnbind);
extern "C" REX_FUNC(OriginalNotification);

namespace {
using namespace forza::challenge_hud;
Mailbox mailbox;
int64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr uint32_t Address(uint32_t disc, uint32_t tu4) { return FORZA_TU4 ? tu4 : disc; }
uint32_t Read(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write(uint8_t* base, uint32_t address, uint32_t value) {
  auto* p = forza::GuestPtr(base, address);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(value >> (24 - i * 8));
}
void Timer(uint8_t* base, uint32_t control, float seconds) {
  uint32_t bits;
  std::memcpy(&bits, &seconds, 4);
  Write(base, control + 0xA4, bits);
}

// These fields belong exclusively to the guest UI thread. Reset ownership when
// a control binds/unbinds or a normal alert replaces our text.
uint32_t owner = 0;
uint64_t bound_revision = 0;
void Release(uint32_t control) {
  if (owner == control) { owner = 0; bound_revision = 0; }
}

class Native {
 public:
  Native(const PPCContext& ctx, uint8_t* base) : ctx_(ctx), base_(base) {
    ctx_.r1.u64 = ctx.r1.u32 - 0x400;
  }
  ~Native() { if (scratch_) REX_KERNEL_MEMORY()->SystemHeapFree(scratch_); }
  uint32_t Call(uint32_t address, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    ctx_.r3.u64 = a; ctx_.r4.u64 = b; ctx_.r5.u64 = c;
    rex::runtime::ResolveIndirectFunction(address)(ctx_, base_);
    return ctx_.r3.u32;
  }
  bool Binding(uint32_t binding, const std::string& value) {
    if (!Call(Address(0x82E73BB8, 0x82F355E8), binding)) return false;
    if (!scratch_) scratch_ = REX_KERNEL_MEMORY()->SystemHeapAlloc(512);
    if (!scratch_) return false;
    const auto text = Text(value);
    for (size_t i = 0; i < text.size(); ++i) {
      auto* p = forza::GuestPtr(base_, scratch_ + 16 + uint32_t(i * 2));
      p[0] = uint8_t(text[i] >> 8); p[1] = uint8_t(text[i]);
    }
    auto* end = forza::GuestPtr(base_, scratch_ + 16 + uint32_t(text.size() * 2));
    end[0] = end[1] = 0;
    const uint32_t object = Call(Address(0x82C0F6C0, 0x824265C8), 16);
    if (!object) return false;
    Write(base_, object, Address(0x82001684, 0x82001674));
    Write(base_, object + 4, 0); Write(base_, object + 8, 0); Write(base_, object + 12, 0);
    Call(Address(0x82CAC2C8, 0x82D6DE20), object, scratch_ + 16, uint32_t(text.size()));
    Call(Address(0x82D35328, 0x82555B90), scratch_, object);
    Call(Address(0x82E78BA0, 0x82F3A728), binding, scratch_);
    Call(Address(0x82DE7330, 0x824CEAB0), scratch_);
    return true;
  }

 private:
  PPCContext ctx_;
  uint8_t* base_;
  uint32_t scratch_ = 0;
};

void Tick(PPCContext& ctx, uint8_t* base, uint32_t control) {
  const auto now = Now();
  const auto message = mailbox.Read(now);
  Native ui(ctx, base);
  if (!message) {
    if (owner == control) {
      ui.Call(Address(0x827EB0D0, 0x8289E808), control, 0);
      Release(control);
    }
    return;
  }
  // A native alert has priority. Do not overwrite it or hide it when our message
  // expires; the current challenge can resume once that control is idle again.
  if (owner != control && Read(base, control + 0xA0) != 0) return;
  const bool start = owner != control;
  if (start || bound_revision != message->revision) {
    const auto lines = NativeLines(*message);
    if (!ui.Binding(control + 0x6C, lines.caption) ||
        !ui.Binding(control + 0x78, lines.headline)) return;
    if (start) {
      ui.Call(Address(0x827E8150, 0x8289B0D8), control, 0);
      const auto show = Read(base, Address(0x83304D68, 0x833F3BE8));
      ui.Call(Address(0x82E74FC8, 0x82F36AB8), control, show);
      Write(base, control + 0xA0, 1);
      Write(base, control + 0xA8, 0);
      Write(base, control + 0xAC, 0);
      owner = control;
    }
    bound_revision = message->revision;
    REXLOG_DEBUG("CHALLENGE HUD: native text revision {}", bound_revision);
  }
  Timer(base, control, float(message->until - now) / 1000.0f);
}
}  // namespace

namespace forza {
void ChallengeHudShow(const std::string& title, const std::string& body, double seconds, bool log) {
  if (log) REXLOG_INFO("HUD: {} | {}", title, body);
  mailbox.Show(title, body, seconds, Now());
}
void ChallengeHudClear() { mailbox.Clear(); }
}  // namespace forza

extern "C" REX_FUNC(HudUpdate) {
  const auto control = ctx.r3.u32;
  // Expiration belongs to the mailbox's monotonic deadline, not UI frame rate.
  if (owner == control) Timer(base, control, 60);
  OriginalUpdate(ctx, base);
  Tick(ctx, base, control);
}
extern "C" REX_FUNC(HudBind) {
  Release(ctx.r3.u32);
  OriginalBind(ctx, base);
}
extern "C" REX_FUNC(HudUnbind) {
  Release(ctx.r3.u32);
  OriginalUnbind(ctx, base);
}
extern "C" REX_FUNC(HudNotification) {
  Release(ctx.r3.u32);
  OriginalNotification(ctx, base);
}
