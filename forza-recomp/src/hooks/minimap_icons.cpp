// Use the game's full-size map atlas instead of its half-size minimap atlas.
#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/logging.h>
#include <cstdlib>
#include <cstring>
#include <string_view>

// tu_address_map: TU4 addresses
#if FORZA_TU4
#define ParseMapProperty sub_82DA8EE0
#define OriginalParseMapProperty __imp__sub_82DA8EE0
#define EraseString sub_824AE5D8
#else
#define ParseMapProperty sub_82CE6CD8
#define OriginalParseMapProperty __imp__sub_82CE6CD8
#define EraseString sub_8247EF40
#endif
extern "C" REX_FUNC(OriginalParseMapProperty);
extern "C" REX_FUNC(EraseString);

namespace {
uint32_t Load32(uint8_t* base, uint32_t address) {
  const auto* p = forza::GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("FORZA_MINIMAP_HD_ICONS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}
}

extern "C" REX_FUNC(ParseMapProperty) {
  const auto object = ctx.r3.u32;
  const bool texture = Enabled() &&
      std::strcmp(reinterpret_cast<const char*>(forza::GuestPtr(base, ctx.r4.u32)), "Texture") == 0;
  OriginalParseMapProperty(ctx, base);
  if (!texture) return;

  // The native symbolizer owns this std::string. Change it before texture lookup,
  // so the resource cache keys and asynchronous loading agree on the same path.
  const uint32_t text = object + 0xA8;
  const auto length = Load32(base, text + 0x10);
  constexpr std::string_view suffix = "\\MapIconSheetSmall.tga";
  if (length < suffix.size() || length > 255) return;
  const auto address = Load32(base, text + 0x14) < 16 ? text : Load32(base, text);
  const std::string_view path(reinterpret_cast<const char*>(forza::GuestPtr(base, address)), length);
  if (!path.ends_with(suffix)) return;

  // Both atlases use the same normalized 8x8 cells and localized artwork.
  // Native erase retains ownership/capacity and removes only "Small".
  PPCContext call = ctx;
  call.r1.u64 = ctx.r1.u32 - 0x200;
  call.r3.u64 = text;
  call.r4.u64 = length - 9;
  call.r5.u64 = 5;
  EraseString(call, base);
  static thread_local bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("minimap: using full-size native map icon atlas");
  }
}
