#pragma once

#include "guest_addr.h"
#include <cstdint>
#include <rex/ppc/context.h>

namespace forza::fh1_map_preview {
#if FORZA_TU4 && defined(FORZA_FH2_PAUSE_SCENE)
// Call on the guest UI thread. Hide before opening a native child screen.
// This samples the existing HUD target; it owns no gameplay camera or profile.
void Update(const PPCContext&, uint8_t* base, uint32_t pause_screen, bool visible,
            bool selected = false);
void Release(const PPCContext&, uint8_t* base);
bool Ready();
#else
inline void Update(const PPCContext&, uint8_t*, uint32_t, bool, bool = false) {}
inline void Release(const PPCContext&, uint8_t*) {}
inline bool Ready() { return false; }
#endif
} // namespace forza::fh1_map_preview
