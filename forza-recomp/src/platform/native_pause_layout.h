#pragma once
#include "menu_model.h"
#include "fh1_menu_components.h"
#include "pause_camera_framing.h"
#include <array>
#include <cmath>

namespace forza::native_pause_layout {
// A bounded native backend, independent of the arbitrary-sized menu model.
// Reject unsupported pages before hiding stock controls.
inline constexpr size_t kTileCapacity = 6;
// Four reusable scene donors form a scrolling window over up to 64 pages.
inline constexpr size_t kTabCapacity = 4;
inline constexpr size_t kPageCapacity = 64;
inline bool Valid(const menu::Rect& r) {
  return std::isfinite(r.x) && std::isfinite(r.y) &&
      std::isfinite(r.width) && std::isfinite(r.height) &&
      r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 &&
      r.x + r.width <= 1280 && r.y + r.height <= 720;
}
inline bool Supports(const std::vector<menu::Page>& pages) {
  if (pages.empty() || pages.size() > kPageCapacity) return false;
  for (const auto& page : pages) {
    if (page.tiles.size() > kTileCapacity) return false;
    for (const auto& tile : page.tiles) if (!Valid(tile.bounds)) return false;
  }
  return true;
}
struct CarContent {
  menu::Rect name, photo, specifications;
};
// Photo and specifications fill one padded composition. The native spec
// values remain localized and keep the player's chosen measurement units.
inline CarContent Car(const menu::Rect& panel) {
  const double inner = panel.width - 48;
  return {{panel.x + 24, panel.y + 62, inner, 42},
          {panel.x + 24, panel.y + 108, inner, panel.height - 190},
          {panel.x + 24, panel.y + panel.height - 66, inner, 46}};
}
inline menu::Rect Map(const menu::Rect& panel) {
  using namespace fh1_menu_components;
  const double x=panel.x+12,y=panel.y+54,w=panel.width-24,h=panel.height-66;
    // The image quad follows the padded panel's two slanted edges. Its texture
    // mapping compensates this shear so map labels and markers stay upright.
    const double width=kMenuScaleX*w;
  const double center=640+kMenuTranslateX+kMenuScaleX*(x+w/2-640)+kMenuShear*(360-y-h/2);
  return {center-width/2,y,width,h};
}
inline constexpr std::array<menu::Rect, kTabCapacity> kTabs{{
    {64, 112, 172, 42}, {244, 112, 172, 42}, {424, 112, 172, 42}, {604, 112, 172, 42}}};
inline bool CameraPage(std::string_view id) {
  // Long social/account/session text keeps its original readable layout and
  // original driving view rather than squeezing those pages around a shot.
  return id=="home"||id=="cars"||id=="settings";
}
inline void CameraPages(std::vector<menu::Page>& pages) {
  for(auto& page:pages) {
    if(!CameraPage(page.id))continue;
    if(page.id=="home"&&page.tiles.size()==6) {
      constexpr std::array<menu::Rect,6> bounds{{{48,166,328,240},{392,166,276,112},
          {392,294,276,112},{48,422,300,120},{48,558,620,76},{364,422,304,120}}};
      for(size_t i=0;i<bounds.size();++i)page.tiles[i].bounds=bounds[i];
    } else if(page.id=="cars"&&page.tiles.size()==5) {
      page.tiles[0].bounds={48,166,328,404};
      for(size_t i=1;i<page.tiles.size();++i)page.tiles[i].bounds={392,166+104.*(i-1),276,92};
    } else {
      for(size_t i=0;i<page.tiles.size();++i)page.tiles[i].bounds={48+316.*(i%2),166+140.*(i/2),300,124};
    }
  }
}
} // namespace forza::native_pause_layout
