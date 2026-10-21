#pragma once
#include <cstdint>
struct PPCContext;
namespace forza::fh2_pause_scene {
bool Owns(uint32_t screen);
bool Input(PPCContext&,uint8_t*);
bool Page(PPCContext&,uint8_t*,uint32_t,bool);
void Destroy(PPCContext&, uint8_t*, uint32_t);
}
