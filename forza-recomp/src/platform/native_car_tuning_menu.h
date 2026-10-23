#pragma once
#include <cstdint>
struct PPCContext;

namespace forza::car_tuning_menu {
inline constexpr uint32_t kMenuAction = 0x7115;
// Native FH1 options page. No missing legacy tuning scene is loaded.
bool Available(const PPCContext&, uint8_t* base, uint32_t pause_screen);
bool Open(const PPCContext&, uint8_t* base, uint32_t pause_screen);
bool TryBind(PPCContext&, uint8_t* base);
bool TryNotify(PPCContext&, uint8_t* base);
bool TryDestroy(PPCContext&, uint8_t* base);
bool TryInput(PPCContext&, uint8_t* base);
void Tick(const PPCContext&, uint8_t* base);
} // namespace forza::car_tuning_menu
