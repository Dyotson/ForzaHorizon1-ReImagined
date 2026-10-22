#pragma once

#include "guest_addr.h"
#include "menu_model.h"
#include <cstdint>
struct PPCContext;

namespace forza::fh1_map_preview {
#if FORZA_TU4 && defined(FORZA_NATIVE_PAUSE)
// Call on the guest UI thread. Hide before opening a native child screen.
// This samples the existing HUD target; it owns no gameplay camera or profile.
// Bounds describe the image in the 1280x720 design canvas. Invalid or
// unrepresentable bounds leave the existing bounds and caption setting intact.
bool SetBounds(const menu::Rect&, bool caption = true);
void Update(const PPCContext&, uint8_t* base, uint32_t pause_screen, bool visible,
            bool selected = false);
void Release(const PPCContext&, uint8_t* base);
bool Ready();
#else
inline bool SetBounds(const menu::Rect&, bool = true) { return false; }
inline void Update(const PPCContext&, uint8_t*, uint32_t, bool, bool = false) {}
inline void Release(const PPCContext&, uint8_t*) {}
inline bool Ready() { return false; }
#endif
} // namespace forza::fh1_map_preview
