// Repository-owned navigation and composition shared by native FH1 menus.
// No tuning fields, guest addresses, serialized scenes or input polling here.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace forza::fh1_menu_components {
inline double SliderGridStart(double minimum,double zero,double quantum) {
  return zero+std::ceil((minimum-zero)/quantum)*quantum;
}
// Index zero denotes stock, rather than a numeric endpoint. Skip values that
// would format identically (including a saved value between generated steps).
inline std::optional<size_t> SliderNeighbor(std::span<const float> choices,
    size_t selected,float stock,float tolerance,int direction) {
  if(choices.size()<2||selected>=choices.size()||!std::isfinite(stock)||
      !std::isfinite(tolerance)||tolerance<0||!direction)return {};
  const float current=selected?choices[selected]:stock;
  auto first=choices.begin()+1,last=choices.end();
  if(direction>0){auto next=std::upper_bound(first,last,current+tolerance);
    return next==last?selected:size_t(next-choices.begin());}
  auto next=std::lower_bound(first,last,current-tolerance);
  return next==first?selected:size_t((next-1)-choices.begin());
}
struct Rect { float x=0,y=0,w=0,h=0; };
struct Tab {
  std::string id, label;
  bool locked=false;
};
struct TabCell { size_t index=0; Rect bounds; bool selected=false,focused=false; };
class TabBar {
  std::vector<Tab> tabs_;
  std::string selected_, focused_;
  size_t Index(std::string_view id) const {
    auto found=std::find_if(tabs_.begin(),tabs_.end(),[&](const Tab& tab){return tab.id==id;});
    return size_t(found-tabs_.begin());
  }
public:
  // Stable IDs preserve selection across capability/label updates. Locked tabs
  // remain selectable so their explanation is reachable with any input device.
  bool SetTabs(std::vector<Tab> tabs) {
    if(tabs.empty() || tabs.size()>64)return false;
    std::unordered_set<std::string> ids;
    for(const auto& tab:tabs)if(tab.id.empty()||tab.label.empty()||!ids.insert(tab.id).second)return false;
    tabs_=std::move(tabs);
    if(Index(selected_)==tabs_.size())selected_=tabs_.front().id;
    if(Index(focused_)==tabs_.size())focused_=selected_;
    return true;
  }
  const std::vector<Tab>& tabs()const{return tabs_;}
  size_t selected_index()const{return Index(selected_);}
  std::string_view selected_id()const{return selected_;}
  bool Select(std::string_view id) {
    if(Index(id)==tabs_.size())return false;
    selected_=focused_=id;return true;
  }
  bool MoveFocus(int direction) {
    if(tabs_.empty()||!direction)return false;
    auto index=Index(focused_);if(index==tabs_.size())index=0;
    index=direction<0?(index+tabs_.size()-1)%tabs_.size():(index+1)%tabs_.size();
    focused_=tabs_[index].id;return true;
  }
  bool ActivateFocused(){return Select(focused_);}
  bool SelectNeighbor(int direction){focused_=selected_;return MoveFocus(direction)&&ActivateFocused();}
  // A bounded viewport supports variable page counts; focus scrolls into view.
  std::vector<TabCell> Layout(Rect bounds,size_t capacity=9)const {
    if(tabs_.empty()||!capacity||!std::isfinite(bounds.w)||bounds.w<=0)return {};
    auto count=std::min(capacity,tabs_.size());auto focused=Index(focused_);
    auto first=focused<count?0:std::min(focused-count+1,tabs_.size()-count);
    float gap=6, width=std::min(220.f,(bounds.w-gap*float(count-1))/float(count));
    if(width<=0)return {};
    std::vector<TabCell> cells;
    for(size_t i=0;i<count;++i)cells.push_back({first+i,{bounds.x+float(i)*(width+gap),bounds.y,width,bounds.h},tabs_[first+i].id==selected_,tabs_[first+i].id==focused_});
    return cells;
  }
  // Binding supplies native donor artwork/text; this renderer is also usable by
  // pause pages without owning or knowing their gameplay controller.
  template<class Paper,class Label>
  void Paint(Rect bounds,size_t capacity,Paper paper,Label label)const {
    auto cells=Layout(bounds,capacity);
    for(size_t slot=0;slot<cells.size();++slot){const auto& cell=cells[slot];const auto& tab=tabs_[cell.index];
      paper(slot,cell.bounds,cell.selected?0xff007au:0x302b30u);
      auto text=cell.bounds;text.x+=4;text.w-=8;text.y+=tab.locked?1:7;text.h=tab.locked?27:31;
      label(slot,0,text,14.f,tab.label,cell.selected?0xffffffu:tab.locked?0xc9c2c9u:0xffffffu);
      if(tab.locked){auto lock=cell.bounds;lock.y+=29;lock.h=17;label(slot,1,lock,11.f,"LOCKED",0xc9c2c9u);}
    }
  }
};

struct Header {
  std::string title,status;
  bool changed=false;
  Rect bounds{42,25,1196,82};
  template<class Paper,class Label,class CarIdentity>
  void Paint(Paper paper,Label label,CarIdentity identity)const {
    paper(bounds,0x302b30u);
    label(0,{bounds.x+22,bounds.y+3,735,45},31.f,title,0xffffffu,0u);
    label(1,{bounds.x+bounds.w-420,bounds.y+10,396,33},18.f,status,changed?0xff83bfu:0xffffffu,2u);
    identity({bounds.x+24,bounds.y+51,220,26},{bounds.x+244,bounds.y+51,bounds.w-520,26},15.f);
  }
};
}
