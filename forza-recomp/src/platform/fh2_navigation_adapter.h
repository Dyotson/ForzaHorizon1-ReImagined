#pragma once

// Execute the private FH2 translations in their own bounded memory arena.
// Only the three datasource virtual calls cross the adapter boundary. FH2
// addresses and object layouts are never installed in FH1 guest memory.
#include <rex/ppc/context.h>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace forza::fh2_navigation {
namespace detail {
constexpr uint32_t kMenu=0x100,kControl=0x600,kSource=0x800,kItems=0xA00;
constexpr uint32_t kStack=0x1F00,kSize=0x2000;
inline uint32_t Read(uint8_t* base,uint32_t address) {
  if(address>kSize-4 || address%4)throw std::out_of_range("FH2 navigation read");
  return uint32_t(base[address])<<24 | uint32_t(base[address+1])<<16 |
         uint32_t(base[address+2])<<8 | base[address+3];
}
inline void Write(uint8_t* base,uint32_t address,uint32_t value) {
  if(address>kSize-4 || address%4)throw std::out_of_range("FH2 navigation write");
  for(int i=3;i>=0;--i) {base[address+i]=uint8_t(value);value>>=8;}
}
inline void Dispatch(uint32_t target,PPCContext& ctx,uint8_t* base) {
  switch(target) {
  case 1:
    if(ctx.r3.u32!=kControl+0x6C)break;
    ctx.r3.u64=kSource;return;
  case 2:
    if(ctx.r3.u32!=kSource || ctx.r4.u32>=4 || ctx.r5.u32!=0)break;
    ctx.r3.u64=kItems+ctx.r4.u32*32;return;
  case 3:
    if(ctx.r3.u32<kItems || ctx.r3.u32>=kItems+128 || (ctx.r3.u32-kItems)%32)break;
    ctx.r3.u64=Read(base,ctx.r3.u32+4);return;
  }
  throw std::runtime_error("Unadapted FH2 navigation call");
}

#define DEFINE_REX_FUNC(name) inline void name(PPCContext& ctx,uint8_t* base)
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LOAD_U32(address) Read(base,uint32_t(address))
#define REX_STORE_U32(address,value) Write(base,uint32_t(address),uint32_t(value))
#define REX_CALL_INDIRECT_FUNC(target) Dispatch(uint32_t(target),ctx,base)
#include "pause_navigation.inc"
#undef DEFINE_REX_FUNC
#undef REX_FUNC_PROLOGUE
#undef REX_LOAD_U32
#undef REX_STORE_U32
#undef REX_CALL_INDIRECT_FUNC
} // namespace detail

inline uint32_t Navigate(uint32_t current,int direction,uint32_t enabled=15) {
  if(current>=4 || (direction!=-1 && direction!=1))
    throw std::invalid_argument("FH2 navigation page or direction");
  using namespace detail;
  std::array<uint8_t,kSize> memory{};
  auto base=memory.data();
  Write(base,kMenu+0x1B8,kControl);Write(base,kMenu+0x3A0,current);
  Write(base,kControl+0x6C,0xC00);Write(base,0xC04,1);
  Write(base,kSource,0xC20);Write(base,0xC28,2);
  Write(base,0xC6C,3);
  for(unsigned i=0;i<4;++i) {
    Write(base,kItems+i*32,0xC40);
    Write(base,kItems+i*32+4,(enabled>>i)&1);
  }
  PPCContext ctx{};ctx.r1.u64=kStack;ctx.r3.u64=kMenu;
  if(direction>0)fh2_pause_next_page(ctx,base);
  else fh2_pause_previous_page(ctx,base);
  return ctx.r3.u32;
}
} // namespace forza::fh2_navigation
