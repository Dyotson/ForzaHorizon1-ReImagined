// Run the actual native action adapter against a synthetic native row vector.
// No translations, game data, SDK or rendering are needed.
#include "../src/platform/fh1_pause_actions.h"
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <vector>

namespace {
std::map<uint32_t,std::array<uint8_t,4096>> pages;
std::vector<uint32_t> calls;
uint32_t selected=0,dispatch_address=0;
constexpr uint32_t screen=0x10000,entries=0x20000,row=0x30000;
void Word(uint32_t address,uint32_t value) {
  auto* p=TestGuestPtr(address);
  for(int i=3;i>=0;--i){p[i]=uint8_t(value);value>>=8;}
}
void Native(PPCContext& ctx,uint8_t*) {
  assert(ctx.r3.u32==screen);
  if(dispatch_address==0x827EBEE0) {
    assert(ctx.r4.u32==2 && ctx.r5.u32==0 && ctx.r6.u32==selected);
  } else assert(dispatch_address==0x827DABD0);
  ctx.r3.u64=0xAAAAAAAA;ctx.r4.u64=0xBBBBBBBB;ctx.r6.u64=0xCCCCCCCC;
}
void Setup(uint32_t id) {
  pages.clear();calls.clear();selected=id;
  Word(screen,0x82061F94);Word(screen+0x19C,entries);Word(screen+0x1A0,entries+4);
  Word(entries,row);Word(row+8,id);Word(row+12,0x40000);TestGuestPtr(row+20)[0]=1;
}
}
uint8_t* TestGuestPtr(uint32_t address){return pages[address>>12].data()+(address&4095);}
namespace rex::runtime {
GuestFunction ResolveIndirectFunction(uint32_t address) {
  dispatch_address=address;calls.push_back(address);return Native;
}
}
int main() {
  using namespace forza::fh1_pause_actions;
  uint8_t memory=0;PPCContext ctx;
  ctx.r3.u64=1;ctx.r4.u64=2;ctx.r5.u64=3;ctx.r6.u64=4;
  const auto original=ctx;
  for(uint32_t id=0;id<20;++id) {
    Setup(id);const auto before=pages;
    assert(Available(&memory,screen,id));
    const auto found=FindRow(&memory,screen,id);
    assert(found && found->address==row && found->text==0x40000 && found->enabled);
    assert(Dispatch(ctx,&memory,screen,id));
    assert((calls==std::vector<uint32_t>{0x827DABD0,0x827EBEE0}));
    assert(std::memcmp(&ctx,&original,sizeof(ctx))==0 && pages==before);
    // Enabled on display, disabled before activation: no native calls.
    calls.clear();TestGuestPtr(row+20)[0]=0;
    assert(!Dispatch(ctx,&memory,screen,id) && calls.empty());
    TestGuestPtr(row+20)[0]=1;Word(screen+0x1A0,entries);
    assert(!Dispatch(ctx,&memory,screen,id) && calls.empty());
  }
  for(int bad=0;bad<6;++bad) {
    Setup(14);
    switch(bad) {
      case 0: Word(screen,0);break;
      case 1: Word(screen+0x19C,0);break;
      case 2: Word(screen+0x1A0,entries-4);break;
      case 3: Word(screen+0x1A0,entries+3);break;
      case 4: Word(screen+0x1A0,entries+1028);break;
      case 5: Word(screen+0x1A0,entries+8);Word(entries+4,row);break;
    }
    assert(!Dispatch(ctx,&memory,screen,14) && calls.empty());
  }
  Setup(14);
  assert(!Dispatch(ctx,nullptr,screen,14) && !Dispatch(ctx,&memory,0,14));
  assert(!Dispatch(ctx,&memory,screen,20) && !Dispatch(ctx,&memory,screen,0x7000));
  assert(calls.empty());
}
