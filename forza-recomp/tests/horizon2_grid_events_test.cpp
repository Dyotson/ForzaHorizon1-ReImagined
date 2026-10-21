#include "../src/platform/fh2_grid_events.h"
#include "../src/platform/fh2_focus_binding.h"
#include <cassert>
#include <map>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static uint32_t EventHash(const std::string& text) {
  uint32_t hash=0;
  for(auto c:text)hash=hash*65599+uint8_t(c);
  return (hash*65599)&0x7fffffff; // Native event hashing includes the terminator.
}
int main() {
  using namespace forza::fh2_grid_events;
  for(uint32_t page=0;page<4;++page) {
    assert(Immediate(page)==EventHash("ON_GRID"+std::to_string(page+1)));
    for(uint32_t to=0;to<4;++to)
      assert(Transition(page,to)==EventHash("PAGE_"+std::to_string(page+1)+"_TO_"+std::to_string(to+1)));
  }
  for(auto invalid:{4u,0xffffffffu}) {
    bool rejected=false;
    try {Immediate(invalid);}catch(const std::invalid_argument&) {rejected=true;}
    assert(rejected);rejected=false;
    try {Transition(0,invalid);}catch(const std::invalid_argument&) {rejected=true;}
    assert(rejected);
  }
  assert(Focus()==EventHash("FOCUSED"));assert(Focus(false)==EventHash("FOCUS"));
  assert(Blur()==EventHash("BLURRED"));assert(Blur(false)==EventHash("BLUR"));
  assert(Select()==EventHash("SELECTED"));assert(Select(false)==EventHash("SELECT"));
  assert(Unselect()==EventHash("UNSELECTED"));assert(Unselect(false)==EventHash("UNSELECT"));
  // Factory order need not be tree order. Prefer the closest button contract,
  // exclude siblings, and bound parent cycles without losing the old focus.
  const uint32_t contracts[] = {40, 30, 50};
  std::map<uint32_t,uint32_t> parents{{40,30},{30,20},{20,10},{50,60},{60,50}};
  auto parent=[&](uint32_t node){return parents[node];};
  assert(forza::fh2_focus_binding::FindContract(20,contracts,parent)==30);
  assert(forza::fh2_focus_binding::FindContract(40,contracts,parent)==40);
  assert(forza::fh2_focus_binding::FindContract(999,contracts,parent)==0);
  std::vector<std::pair<uint32_t,uint32_t>> events;
  auto emit=[&](uint32_t node,uint32_t event){events.emplace_back(node,event);};
  forza::fh2_focus_binding::Focus focus;
  assert(focus.Bind(20,contracts,parent,emit));
  assert(events.size()==1 && events[0]==std::make_pair(30u,EventHash("FOCUSED")));
  assert(focus.Bind(20,contracts,parent,emit) && events.size()==1);
  assert(!focus.Bind(999,contracts,parent,emit) && focus.current()==30);
  focus.Set(40,emit,false);
  assert(events.size()==3 && events[1]==std::make_pair(30u,EventHash("BLUR")) &&
         events[2]==std::make_pair(40u,EventHash("FOCUS")));
  focus.Set(0,emit);
  assert(events.size()==4 && events[3]==std::make_pair(40u,EventHash("BLURRED")));
  focus.Set(30,emit);focus.Forget();
  assert(events.size()==5 && focus.current()==0);
  struct NativeColor {
    uint8_t* base;
    uint32_t address=0;
    std::vector<uint32_t> args;
    void Call(uint32_t target,std::initializer_list<uint32_t> values) {
      address=target;args=values;
    }
  };
  std::array<uint8_t,64> color_memory{};
  NativeColor native{color_memory.data()};
  assert(forza::fh2_focus_binding::SetMaterialRgb(native,0x1234,0x5678,16,0xAAED127A));
  auto read_color=[&](unsigned offset) {
    const auto* p=color_memory.data()+offset;
    return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];
  };
  assert(read_color(16)==0x820307A4 && read_color(20)==0x1234 && read_color(24)==0x5678);
  assert(native.address==0x82F37C40 && native.args==std::vector<uint32_t>({16,0xED127A}));
  assert(!forza::fh2_focus_binding::SetMaterialRgb(native,0,0x5678,16,0xED127A));
  std::puts("Original FH2 grid/button routines: 28 event cases and invalid pages passed; ABI preserved.");
  std::puts("Native contract ancestry, focus changes and teardown passed.");
}
