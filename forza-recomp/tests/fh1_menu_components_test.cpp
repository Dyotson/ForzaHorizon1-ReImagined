#include "../src/platform/fh1_menu_components.h"
#include <cstdlib>
#include <iostream>
using namespace forza::fh1_menu_components;
void Check(bool condition,const char* message){if(!condition){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
  const auto grid=SliderGridStart(188.55,0,.1);
  Check(std::fabs(grid-188.6)<1e-8,"Choice grid must align to display precision, not raw minimum");
  Check(std::fabs((grid+489*1.3)-824.3)<1e-8,"Spring choices must avoid half-decimal rounding midpoints");
  const std::vector<float> choices{-1,15,20,29.9f,30,30.00001f,30.1f,55};
  Check(SliderNeighbor(choices,0,30,.025f,1)==6,"Stock right must step above stock");
  Check(SliderNeighbor(choices,0,30,.025f,-1)==3,"Stock left must step below stock");
  Check(SliderNeighbor(choices,4,30,.025f,1)==6,"Saved near-duplicate must not consume an input step");
  Check(SliderNeighbor(choices,1,30,.025f,-1)==1,"Minimum must clamp instead of resetting to stock");
  Check(SliderNeighbor(choices,7,30,.025f,1)==7,"Maximum must clamp");
  Check(SliderNeighbor(choices,0,15,.025f,-1)==0,"Bounded stock must preserve its sentinel");
  Check(!SliderNeighbor(choices,99,30,.025f,1),"Invalid slider selection must fail closed");
  TabBar tabs;
  Check(!tabs.Select("missing")&&tabs.Layout({0,0,100,40}).empty(),"Empty navigation must remain inert");
  Check(tabs.SetTabs({{"a","A"},{"locked","Locked",true},{"c","C"}}),"Valid tabs rejected");
  tabs.SelectNeighbor(-1);Check(tabs.selected_id()=="c","Previous must wrap");
  tabs.SelectNeighbor(1);Check(tabs.selected_id()=="a","Next must wrap");
  tabs.MoveFocus(1);Check(tabs.selected_id()=="a","Directional focus must not activate a page");
  tabs.ActivateFocused();Check(tabs.selected_id()=="locked","Locked page explanation must remain reachable");
  Check(tabs.SetTabs({{"c","Renamed"},{"locked","Still locked",true}})&&tabs.selected_id()=="locked","Reorder/rename lost stable selection");
  Check(!tabs.SetTabs({{"x","X"},{"x","Duplicate"}})&&tabs.selected_id()=="locked","Invalid update corrupted selection");
  Check(tabs.SetTabs({{"c","C"}})&&tabs.selected_id()=="c","Removed selection did not fall back");
  std::vector<Tab> many;for(int i=0;i<12;++i)many.push_back({std::to_string(i),"Page"});
  tabs.SetTabs(many);tabs.Select("11");auto cells=tabs.Layout({44,120,1192,48},9);
  Check(cells.size()==9&&cells.back().index==11&&cells.back().selected,"Viewport did not expose selected page");
  Check(cells.front().bounds.x>=44&&cells.back().bounds.x+cells.back().bounds.w<=1236.01,"Viewport exceeded supplied bounds");
  Check(tabs.Layout({0,0,2,48},9).empty(),"Unsupported narrow layout must fail closed");
}
