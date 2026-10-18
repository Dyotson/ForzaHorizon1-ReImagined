// The native map's ellipse colour pass uses a zero source-alpha factor to
// preserve destination alpha.
// MAX ignores blend factors on the host, so the opaque barn-find colour pass
// otherwise sets alpha to 1 before its separate 20% alpha pass can run (#111).
// tu_address_map: TU4 addresses
#include "../platform/guest_addr.h"
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <cstdlib>
#include <cstring>

#if FORZA_TU4
#define MapRenderState sub_82497228
#define OriginalMapRenderState __imp__sub_82497228
#else
#define MapRenderState sub_824679E0
#define OriginalMapRenderState __imp__sub_824679E0
#endif
extern "C" REX_FUNC(OriginalMapRenderState);

extern "C" REX_FUNC(MapRenderState) {
  // r16 is the map renderer's ellipse-technique flag in both executables.
  // Roads also use a zero factor, but must keep their existing MAX alpha.
  const bool map_alpha = ctx.lr == (FORZA_TU4 ? 0x8247ADEC : 0x8244B72C) &&
                         ctx.r16.u32 == 1 && ctx.r4.u32 == 0x54 && ctx.r5.u32 <= 1;
  const auto states = ctx.r3.u32;
  const auto alpha = ctx.r5.u32;
  OriginalMapRenderState(ctx, base);
  static const bool enabled = [] {
    const char* value = std::getenv("FORZA_MAP_ALPHA_FIX");
    return !value || std::strcmp(value, "0") != 0;
  }();
  if (!map_alpha || !enabled) return;

  // ADD(0, destination) preserves alpha for the ellipse colour pass. Its
  // ellipse_alpha pass restores MAX before subsequent roads and icons.
  // Push this through the same native state scope so its normal unwind restores
  // the previous equation; don't leave a changed blend mode for other HUD draws.
  PPCContext call = ctx;
  call.r1.u64 = ctx.r1.u32 - 0x400;
  call.r3.u64 = states;
  call.r4.u64 = 0x5C;  // native alpha blend operation
  call.r5.u64 = alpha ? 3 : 0;  // Xenos MAX / ADD
  OriginalMapRenderState(call, base);
  static thread_local bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("map alpha: preserving alpha for barn-find ellipse colour passes (#111)");
  }
}
