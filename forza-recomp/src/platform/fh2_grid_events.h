#pragma once

// The original CGridMenuCtrl immediate/transition methods execute in isolated
// memory. Only their resulting event hash crosses into FH1's scene engine.
// This does not install FH2 vtables, allocators, or object layouts in FH1.
#include <rex/ppc/context.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace forza::fh2_grid_events {
namespace detail {
constexpr uint32_t kSize=0x400,kStack=0x300,kControl=0x40,kOutput=0x20;
constexpr uint32_t StateHash(const char* text) {
  uint32_t value=0;
  do {value=value*65599+uint8_t(*text);}while(*text++);
  return value&0x7fffffff;
}
inline void Bounds(uint32_t address,unsigned count) {
  if(count>kSize || address>kSize-count)throw std::out_of_range("FH2 grid memory");
}
inline uint64_t Read(uint8_t* base,uint32_t address,unsigned count) {
  // Read-only FH2 globals, initialized from their original startup event
  // strings (83241258..832414F4). These never address FH1 guest memory.
  if(count==4)switch(address) {
  case 0x8342446C:return StateHash("FOCUSED");
  case 0x83424480:return StateHash("FOCUS");
  case 0x83424440:return StateHash("BLURRED");
  case 0x83424420:return StateHash("BLUR");
  case 0x83424464:return StateHash("SELECTED");
  case 0x83424458:return StateHash("SELECT");
  case 0x83424478:return StateHash("UNSELECTED");
  case 0x83424454:return StateHash("UNSELECT");
  }
  Bounds(address,count);uint64_t value=0;
  for(unsigned i=0;i<count;++i)value=(value<<8)|base[address+i];
  return value;
}
inline void Write(uint8_t* base,uint32_t address,uint64_t value,unsigned count) {
  Bounds(address,count);
  for(unsigned i=count;i>0;--i) {base[address+i-1]=uint8_t(value);value>>=8;}
}
inline void GridFormat(PPCContext& ctx,uint8_t* base) {
  auto destination=ctx.r3.u32;Bounds(destination,32);
  int length;
  if(ctx.r4.u32==0x8208EEF0)
    length=std::snprintf(reinterpret_cast<char*>(base+destination),32,"ON_GRID%u",ctx.r5.u32);
  else if(ctx.r4.u32==0x8208EEFC)
    length=std::snprintf(reinterpret_cast<char*>(base+destination),32,"PAGE_%u_TO_%u",ctx.r5.u32,ctx.r6.u32);
  else throw std::runtime_error("Unexpected FH2 grid format");
  if(length<0 || length>=32)throw std::runtime_error("FH2 grid format overflow");
  ctx.r3.u64=uint32_t(length);
}
inline void GridEmit(PPCContext& ctx,uint8_t* base) {
  if(ctx.r3.u32!=kControl)throw std::runtime_error("Unexpected FH2 grid receiver");
  Write(base,kOutput,ctx.r4.u32,4);
  Write(base,kOutput+4,Read(base,kOutput+4,4)+1,4);
}
#define DEFINE_REX_FUNC(name) inline void name(PPCContext& ctx,uint8_t* base)
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LOAD_U8(address) uint8_t(Read(base,uint32_t(address),1))
#define REX_LOAD_U32(address) uint32_t(Read(base,uint32_t(address),4))
#define REX_LOAD_U64(address) Read(base,uint32_t(address),8)
#define REX_STORE_U32(address,value) Write(base,uint32_t(address),uint32_t(value),4)
#define REX_STORE_U64(address,value) Write(base,uint32_t(address),uint64_t(value),8)
#include "grid_events.inc"
#undef DEFINE_REX_FUNC
#undef REX_FUNC_PROLOGUE
#undef REX_LOAD_U8
#undef REX_LOAD_U32
#undef REX_LOAD_U64
#undef REX_STORE_U32
#undef REX_STORE_U64

inline uint32_t Run(uint32_t from,uint32_t to,bool immediate) {
  if(from>=4 || to>=4)throw std::invalid_argument("FH2 grid page");
  std::array<uint8_t,kSize> memory{};auto base=memory.data();
  PPCContext ctx{};ctx.r1.u64=kStack;ctx.r3.u64=kControl;
  ctx.lr=0x11223344;ctx.r31.u64=0x123456789ABCDEF0ull;
  ctx.r4.u64=immediate?to:from;ctx.r5.u64=to;
  if(immediate)fh2_grid_immediate(ctx,base);else fh2_grid_transition(ctx,base);
  if(ctx.r1.u32!=kStack || ctx.lr!=0x11223344 || ctx.r31.u64!=0x123456789ABCDEF0ull ||
     Read(base,kOutput+4,4)!=1)throw std::runtime_error("FH2 grid ABI mismatch");
  return uint32_t(Read(base,kOutput,4));
}
using ButtonMethod=void(*)(PPCContext&,uint8_t*);
inline uint32_t RunButton(ButtonMethod method,bool immediate) {
  std::array<uint8_t,kSize> memory{};auto base=memory.data();
  PPCContext ctx{};ctx.r1.u64=kStack;ctx.r3.u64=kControl;ctx.r4.u64=immediate;
  ctx.lr=0x11223344;ctx.r31.u64=0x123456789ABCDEF0ull;
  method(ctx,base);
  if(ctx.r1.u32!=kStack || ctx.lr!=0x11223344 || ctx.r31.u64!=0x123456789ABCDEF0ull ||
     Read(base,kOutput+4,4)!=1)throw std::runtime_error("FH2 button ABI mismatch");
  return uint32_t(Read(base,kOutput,4));
}
} // namespace detail
inline uint32_t Immediate(uint32_t page) {return detail::Run(page,page,true);}
inline uint32_t Transition(uint32_t from,uint32_t to) {return detail::Run(from,to,false);}
inline uint32_t Focus(bool immediate=true) {return detail::RunButton(detail::fh2_button_focus,immediate);}
inline uint32_t Blur(bool immediate=true) {return detail::RunButton(detail::fh2_button_blur,immediate);}
inline uint32_t Select(bool immediate=true) {return detail::RunButton(detail::fh2_button_select,immediate);}
inline uint32_t Unselect(bool immediate=true) {return detail::RunButton(detail::fh2_button_unselect,immediate);}
} // namespace forza::fh2_grid_events
