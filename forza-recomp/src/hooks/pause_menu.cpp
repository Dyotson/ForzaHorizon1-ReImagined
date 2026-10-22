// FH2 page navigation adapted to the TU4 native FH1 pause view and actions.
// tu_address_map: TU4 addresses
#include "../platform/pause_menu.h"
#include "../platform/fh2_pause_scene.h"
#include "../platform/guest_addr.h"
#if FORZA_TU4 && defined(FORZA_FH2_PAUSE)
#include "../platform/guest_memory.h"
#include "../platform/fh2_navigation_adapter.h"
#include "../platform/offline_cars.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

// Keep the legacy native-list view opt-in while the actual FH2 scene is ported.
REXCVAR_DEFINE_BOOL(forza_tabbed_pause,false,"Game/UI",
                   "Experimental FH2 navigation using the legacy FH1 pause view");
extern "C" REX_FUNC(__imp__sub_827D1618);
extern "C" REX_FUNC(__imp__sub_827DADC8);
extern "C" REX_FUNC(__imp__sub_827D6248);
extern "C" REX_FUNC(__imp__XamInputGetState);

namespace forza::pause_menu {
namespace {
constexpr uint32_t kDifficulty=0x7104,kCars=0x7108,kGraphics=0x7000;
constexpr std::array<const char*,4> kNames={"HOME","MAP","CARS","SETTINGS"};
uint32_t Read(uint8_t* base,uint32_t address) {
  const auto p=GuestPtr(base,address);
  return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
void Write(uint8_t* base,uint32_t address,uint32_t value) {
  auto p=GuestPtr(base,address);
  for(int i=3;i>=0;--i) {p[i]=uint8_t(value);value>>=8;}
}
class Native {
public:
  PPCContext ctx;
  uint8_t* base;
  uint32_t scratch;
  Native(const PPCContext& context,uint8_t* memory):ctx(context),base(memory),
      scratch(REX_KERNEL_MEMORY()->SystemHeapAlloc(1024)) {}
  ~Native() {if(scratch)REX_KERNEL_MEMORY()->SystemHeapFree(scratch);}
  uint32_t Call(uint32_t address,std::initializer_list<uint32_t> args) {
    unsigned i=0;
    for(auto value:args)switch(i++) {
    case 0:ctx.r3.u64=value;break;case 1:ctx.r4.u64=value;break;
    case 2:ctx.r5.u64=value;break;case 3:ctx.r6.u64=value;break;
    case 4:ctx.r7.u64=value;break;case 5:ctx.r8.u64=value;break;
    }
    rex::runtime::ResolveIndirectFunction(address)(ctx,base);
    return ctx.r3.u32;
  }
  uint32_t Text(const std::string& label) {
    if(label.size()>240)return 0;
    auto out=GuestPtr(base,scratch+32);
    for(size_t i=0;i<label.size();++i) {out[i*2]=0;out[i*2+1]=uint8_t(label[i]);}
    out[label.size()*2]=out[label.size()*2+1]=0;
    auto object=Call(0x824265C8,{16});
    if(!object)return 0;
    Write(base,object,0x82001674);
    for(unsigned i=4;i<16;i+=4)Write(base,object+i,0);
    Call(0x82D6DE20,{object,scratch+32,uint32_t(label.size())});
    Call(0x82555B90,{scratch,object});
    return scratch;
  }
  void Add(uint32_t menu,uint32_t id,const char* label,bool enabled=true) {
    if(auto text=Text(label)) {
      Call(0x828F5818,{menu+0x174,text,id,uint32_t(enabled),0});
      Call(0x824CEAB0,{text});
    }
  }
};
struct Row {uint32_t pointer,id;};
uint32_t screen=0,page=0;
std::optional<uint32_t> return_page;
std::vector<Row> rows;
uint16_t previous_shoulders=0;
bool visible=true;
unsigned selection_delay=0;
uint32_t Category(uint32_t id) {
  if(id==2)return 1;
  if(id==kCars)return 2;
  if(id==kDifficulty || id==kGraphics)return 3;
  return 0;
}
void Publish(uint8_t* base,bool all=false) {
  auto begin=Read(base,screen+0x19C),end=begin;
  if(!all && page==3)for(auto row:rows)if(row.id==kDifficulty) {
    Write(base,end,row.pointer);end+=4;
  }
  for(auto row:rows)if(all || (Category(row.id)==page &&
                             !(page==3 && row.id==kDifficulty))) {
    Write(base,end,row.pointer);end+=4;
  }
  Write(base,screen+0x1A0,end);
}
void Decorate(Native& ui) {
  ui.Call(0x827D6348,{screen});
  auto footer=ui.Call(0x82746638,{screen});
  if(footer) {
    for(auto [button,label]:{std::pair{8u,"PREVIOUS TAB"},std::pair{9u,"NEXT TAB"}})
      if(auto text=ui.Text(label)) {
        ui.Call(0x8289E2B0,{footer,button,text});
        ui.Call(0x824CEAB0,{text});
      }
    ui.Call(Read(ui.base,Read(ui.base,footer+0x6C)+0x28),{footer+0x6C});
  }
  REXLOG_INFO("Pause tabs: {}",kNames[page]);
}
void Change(PPCContext& ctx,uint8_t* base,int direction) {
  auto next=fh2_navigation::Navigate(page,direction);
  if(next==page)return;
  Native ui(ctx,base);if(!ui.scratch)return;
  page=next;Publish(base);Decorate(ui);
  ui.ctx.r3.u64=screen;__imp__sub_827D1618(ui.ctx,base);
  auto component=Read(base,screen+0x170);
  ui.Call(0x828ADF38,{component,0});
  ui.Call(Read(base,Read(base,component+0x6C)+0x28),{component+0x6C});
  Decorate(ui);
  selection_delay=3;
}
void Reset(uint8_t* base) {
  if(screen)Publish(base,true); // The native destructor owns every row.
  screen=0;rows.clear();previous_shoulders=0;visible=true;selection_delay=0;
}
}
void Bind(PPCContext& ctx,uint8_t* base) {
  if(!REXCVAR_GET(forza_tabbed_pause) || screen==ctx.r3.u32)return;
  auto menu=ctx.r3.u32;
  auto first=Read(base,menu+0x19C),end=Read(base,menu+0x1A0);
  if(!first || end<first || end-first>128 || (end-first)%4)return;
  bool has_map=false;
  for(auto p=first;p<end;p+=4) {
    auto row=Read(base,p);if(!row)return;
    has_map|=Read(base,row+8)==2;
  }
  if(!has_map)return; // Race and split-screen pauses keep their own behavior.
  Native ui(ctx,base);if(!ui.scratch)return;
  if(screen)Reset(base);
  ui.Add(menu,kDifficulty,"DIFFICULTY");
  ui.Add(menu,kCars,"CHANGE CAR");
  screen=menu;page=return_page.value_or(0);return_page.reset();visible=true;
  first=Read(base,menu+0x19C);end=Read(base,menu+0x1A0);
  for(auto p=first;p<end;p+=4) {
    auto row=Read(base,p);rows.push_back({row,Read(base,row+8)});
  }
  Publish(base);
  Decorate(ui);
  selection_delay=3;
}
void AfterBind(PPCContext& ctx,uint8_t* base) {
  if(ctx.r3.u32!=screen)return;
  Native ui(ctx,base);if(ui.scratch)Decorate(ui);
  if(ui.scratch) {
    auto component=Read(base,screen+0x170);
    ui.Call(0x828ADF38,{component,0});
    ui.Call(Read(base,Read(base,component+0x6C)+0x28),{component+0x6C});
  }
}
bool Select(PPCContext& ctx,uint8_t* base) {
  if(ctx.r3.u32!=screen || ctx.r4.u32!=2)return false;
  auto id=ctx.r6.u32;
  if(id==kCars) {
    if(offline_cars::Open(ctx,base,screen))return_page=page;
    return true;
  }
  REXLOG_INFO("Pause tabs: action={} page={}",id,page);
  if(id==2 || id==12)return_page=page;
  if(id!=kDifficulty)return false;
  Native ui(ctx,base);if(!ui.scratch)return true;
  auto params=ui.Call(0x824265C8,{0x88});if(!params)return true;
  ui.Call(0x825A5640,{params,0});
  if(auto text=ui.Text("DIFFICULTY")) {
    ui.Call(0x82471820,{params+0xC,text});ui.Call(0x824CEAB0,{text});
  }
  Write(base,ui.scratch,params);
  ui.Call(0x82675B00,{screen,5,ui.scratch,0});
  return true;
}
void Page(uint8_t*,uint32_t menu,bool show) {
  if(screen==menu) {visible=show;previous_shoulders=0;if(show)selection_delay=3;}
}
void Tick(PPCContext& ctx,uint8_t* base) {
  if(!screen || !visible || !selection_delay)return;
  if(--selection_delay)return;
  auto component=Read(base,screen+0x170);
  auto list=component?Read(base,component+0x140):0;
  if(!list) {selection_delay=1;return;}
  Native ui(ctx,base);if(!ui.scratch) {selection_delay=1;return;}
  ui.Call(0x82865BF8,{list,0,0});
  REXLOG_INFO("Pause tabs: page={} selection={}",page,Read(base,list+0x15C));
}
} // namespace forza::pause_menu

extern "C" REX_FUNC(sub_827DADC8) {
  if(forza::fh2_pause_scene::Input(ctx,base))return;
  using namespace forza::pause_menu;
  if(screen && ctx.r3.u32==screen && visible) {
    Native ui(ctx,base);
    if(ui.scratch) {
      ui.ctx.r3.u64=0;ui.ctx.r4.u64=0;ui.ctx.r5.u64=ui.scratch;
      __imp__XamInputGetState(ui.ctx,base);
      auto held=ui.ctx.r3.u32==0?uint16_t((Read(base,ui.scratch+4)>>16)&0x300):0;
      auto pressed=uint16_t(held&~previous_shoulders);previous_shoulders=held;
      if(pressed==0x100 || pressed==0x200) {
        Change(ctx,base,pressed==0x100?-1:1);return;
      }
    }
  }
  __imp__sub_827DADC8(ctx,base);
}
extern "C" REX_FUNC(sub_827D6248) {
  const auto scene_owner=ctx.r3.u32;
  using namespace forza::pause_menu;
  if(ctx.r3.u32==screen)Reset(base);
  __imp__sub_827D6248(ctx,base);
  forza::fh2_pause_scene::Destroy(ctx, base, scene_owner);
}
#else
namespace forza::pause_menu {
void Bind(PPCContext&,uint8_t*) {}
void AfterBind(PPCContext&,uint8_t*) {}
void Tick(PPCContext&,uint8_t*) {}
bool Select(PPCContext&,uint8_t*) {return false;}
void Page(uint8_t*,uint32_t,bool) {}
}
#endif
