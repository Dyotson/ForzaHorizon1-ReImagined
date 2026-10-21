#pragma once
#include <cstdint>
struct PPCContext;
namespace forza::pause_menu {
void Bind(PPCContext&,uint8_t*);
void AfterBind(PPCContext&,uint8_t*);
void Tick(PPCContext&,uint8_t*);
bool Select(PPCContext&,uint8_t*);
void Page(uint8_t*,uint32_t,bool);
}
