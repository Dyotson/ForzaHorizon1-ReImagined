// The game's CHudCombinedNotificationControl: headline and caption, using
// native bindings and SHOW/HIDE animations. Never register an ImGui dialog.
// tu_address_map: TU4 addresses
#include "../platform/challenge_hud.h"
#include "../platform/achievement_toast.h"
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <array>
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
bool owner_achievement = false;


class Native {
 public:
  Native(const PPCContext& ctx, uint8_t* base) : ctx_(ctx), base_(base) {
    ctx_.r1.u64 = ctx.r1.u32 - 0x400;
  }
  ~Native() { if (scratch_) REX_KERNEL_MEMORY()->SystemHeapFree(scratch_); }
  uint32_t Call(uint32_t address, uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    ctx_.r3.u64 = a; ctx_.r4.u64 = b; ctx_.r5.u64 = c; ctx_.r6.u64 = d;
    rex::runtime::ResolveIndirectFunction(address)(ctx_, base_);
    return ctx_.r3.u32;
  }
  bool Binding(uint32_t binding, const std::string& value) {
    if (!Call(Address(0x82E73BB8, 0x82F355E8), binding)) return false;
    if (!scratch_) scratch_ = REX_KERNEL_MEMORY()->SystemHeapAlloc(1024);
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

  uint32_t Element(uint32_t control, const std::string& path) {
    if (!scratch_) scratch_ = REX_KERNEL_MEMORY()->SystemHeapAlloc(1024);
    if (!scratch_ || path.size() >= 400) return 0;
    const auto native = Read(base_, control + 0x54);
    const auto scene = Read(base_, native + 8);
    const auto engine = Call(Read(base_, Read(base_, scene) + 0x1C), scene);
    std::memcpy(forza::GuestPtr(base_, scratch_ + 544), path.c_str(), path.size() + 1);
    return Call(Address(0x82E72E68, 0x82F348B8), engine,
                Read(base_, native + 12), scratch_ + 544);
  }
  void Texture(uint32_t control, uint32_t material, const char* uri) {
    const auto scene = Read(base_, Read(base_, control + 0x54) + 8);
    Write(base_, scratch_ + 512, 0);
    Write(base_, scratch_ + 516, material);
    Write(base_, scratch_ + 520, scene);
    std::memcpy(forza::GuestPtr(base_, scratch_ + 544), uri, std::strlen(uri) + 1);
    Call(Address(0x82E773F0, 0x82F38E80), scratch_ + 512, scratch_ + 544, 0, 8);
  }
  void Retain(uint32_t resource) {
    if (resource) Call(Read(base_, Read(base_, resource) + 8), resource);
  }
  void Drop(uint32_t resource) {
    if (resource) Call(Read(base_, Read(base_, resource) + 12), resource);
  }

 private:
  PPCContext ctx_;
  uint8_t* base_;
  uint32_t scratch_ = 0;
};

// Anark's model runtime: local position/scale and opacity are cached here.
// Static properties (such as these icons' positions) have no scalar binding.
// Match the native model-property updater's transform/opacity dirty bits at +FB.
// Keep the original resource alive and restore it before any normal alert/unbind.
struct Icon {
  uint32_t element = 0, model = 0, texture_slot = 0, resource = 0;
  std::array<uint32_t, 5> saved{};
};
std::array<Icon, 2> icons;
bool trophy_icons = false;
constexpr std::array<uint32_t, 5> icon_fields{0x0C, 0x10, 0x24, 0x28, 0xE0};
void Dirty(uint8_t* base, uint32_t model) {
  forza::GuestPtr(base, model + 0xFB)[0] |= 0xA0;
}
void RestoreIcons(Native& ui, uint8_t* base) {
  for (auto& icon : icons) {
    if (!icon.model) continue;
    for (size_t i = 0; i < icon_fields.size(); ++i)
      Write(base, icon.model + icon_fields[i], icon.saved[i]);
    // The active mode's native opacity may differ from the mode preceding SHOW.
    // We only override the render cache, so the native property remains authoritative.
    const auto opacity = ui.Call(Address(0x82F2DDC8, 0x82FEF130), icon.element, 0x0191C315);
    if (opacity) {
      uint32_t bits = Read(base, opacity + 4);
      float value;
      std::memcpy(&value, &bits, 4);
      value *= 0.01f;
      std::memcpy(&bits, &value, 4);
      Write(base, icon.model + 0xE0, bits);
    }
    Dirty(base, icon.model);
    const auto current = Read(base, icon.texture_slot);
    if (current != icon.resource) {
      ui.Retain(icon.resource);
      Write(base, icon.texture_slot, icon.resource);
      ui.Drop(current);
    }
    ui.Drop(icon.resource);
    icon = {};
  }
}
void NoticeIcons(Native& ui, uint8_t* base, uint32_t control, Actions actions, bool trophy) {
  if (trophy_icons != trophy) { RestoreIcons(ui, base); trophy_icons = trophy; }
  if (actions == Actions::kNone && !trophy) { RestoreIcons(ui, base); return; }
  for (size_t i = 0; i < icons.size(); ++i) {
    if (trophy && i != 0) break;
    auto& icon = icons[i];
    if (!icon.model) {
      const std::string path = std::string("parent.State.DoubleTape.Icons.") +
                               (i ? "MailIcon" : "MiscIcon");
      const auto element = ui.Element(control, path);
      const auto material = ui.Element(control, path + ".Material8333");
      if (!element || !material) { RestoreIcons(ui, base); return; }
      const auto object = Read(base, Read(base, material + 4));
      const auto textures = Read(base, object + 0xDC);
      if (!textures || Read(base, object + 0xE0) <= textures) {
        RestoreIcons(ui, base); return;
      }
      icon.element = element;
      icon.model = Read(base, element + 4);
      icon.texture_slot = Read(base, textures) + 0xF4;
      icon.resource = Read(base, icon.texture_slot);
      for (size_t n = 0; n < icon_fields.size(); ++n)
        icon.saved[n] = Read(base, icon.model + icon_fields[n]);
      ui.Retain(icon.resource);
      ui.Texture(control, material, trophy
        ? "GAME:\\Media\\UI\\Textures\\Horizon\\Hud\\Trophy.tga" : i
        ? "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\B_Button.tga"
        : "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\X_Button.tga");
    }
    // Match the caption's visual centre and leave the same six-pixel gap
    // between each 20-pixel button and its label (native 720p coordinates).
    const std::array<float, 5> buttons{
        i ? -37.0f : -142.0f, -14.0f, 0.23333333f, 0.23333333f,
        i && actions != Actions::kAnswer ? 0.0f : 1.0f};
    for (size_t n = 0; n < icon_fields.size(); ++n) {
      uint32_t value;
      std::memcpy(&value, &buttons[n], 4);
      if (trophy) value = n == 4 ? 0x3F800000 : icon.saved[n];
      Write(base, icon.model + icon_fields[n], value);
    }
    Dirty(base, icon.model);
  }
}
void Release(PPCContext& ctx, uint8_t* base, uint32_t control) {
  if (owner != control) return;
  if (owner_achievement) forza::achievement_hud::Notifications().Pause(Now());
  owner_achievement = false;
  Native ui(ctx, base);
  RestoreIcons(ui, base);
  owner = 0;
  bound_revision = 0;
}

void Tick(PPCContext& ctx, uint8_t* base, uint32_t control) {
  const auto now = Now();
  auto message = mailbox.Read(now);
  auto& achievements = forza::achievement_hud::Notifications();
  Native ui(ctx, base);
  bool achievement = false;
  if (message) {
    achievements.Pause(now);  // answering a race request takes priority over an unlock
  } else {
    if (owner == control && !owner_achievement) {
      ui.Call(Address(0x827EB0D0, 0x8289E808), control, 0);
      Release(ctx, base, control);
      return;
    }
    if (owner != control && Read(base, control + 0xA0) != 0) return;
    if (const auto notice = achievements.Read(now)) {
      Message unlocked;
      unlocked.title = "ACHIEVEMENT UNLOCKED";
      unlocked.first = notice->name;
      unlocked.second = "ACHIEVEMENT UNLOCKED  " + std::to_string(notice->gamerscore) + " G";
      unlocked.until = now + notice->remaining;
      unlocked.revision = notice->revision;
      message = std::move(unlocked);
      achievement = true;
    }
  }
  if (!message) {
    if (owner == control) {
      ui.Call(Address(0x827EB0D0, 0x8289E808), control, 0);
      Release(ctx, base, control);
    }
    return;
  }
  // A native alert has priority. Do not overwrite it or hide it when our message
  // expires; the current challenge can resume once that control is idle again.
  if (owner != control && Read(base, control + 0xA0) != 0) return;
  const bool start = owner != control;
  if (start || owner_achievement != achievement || bound_revision != message->revision) {
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
    if (achievement) REXLOG_INFO("ACHIEVEMENT HUD: displaying {}", message->first);
    owner_achievement = achievement;
    bound_revision = message->revision;
    REXLOG_DEBUG("CHALLENGE HUD: native text revision {}", bound_revision);
  }
  NoticeIcons(ui, base, control, message->actions, achievement);
  if (achievement) achievements.Resume(now);
  Timer(base, control, float(message->until - now) / 1000.0f);
}
}  // namespace

namespace forza {
void WorldChallengeHudTick(PPCContext&, uint8_t*);
void ChallengeHudShow(const std::string& title, const std::string& body, double seconds, bool log, challenge_hud::Actions actions) {
  if (log) REXLOG_INFO("HUD: {} | {}", title, body);
  mailbox.Show(title, body, seconds, Now(), actions);
}
void ChallengeHudClear() { mailbox.Clear(); }
}  // namespace forza

extern "C" REX_FUNC(HudUpdate) {
  const auto control = ctx.r3.u32;
  // Expiration belongs to the mailbox's monotonic deadline, not UI frame rate.
  if (owner == control) Timer(base, control, 60);
  OriginalUpdate(ctx, base);
  forza::WorldChallengeHudTick(ctx, base);
  Tick(ctx, base, control);
}
extern "C" REX_FUNC(HudBind) {
  Release(ctx, base, ctx.r3.u32);
  OriginalBind(ctx, base);
}
extern "C" REX_FUNC(HudUnbind) {
  Release(ctx, base, ctx.r3.u32);
  OriginalUnbind(ctx, base);
}
extern "C" REX_FUNC(HudNotification) {
  Release(ctx, base, ctx.r3.u32);
  OriginalNotification(ctx, base);
}
