#pragma once
#include <cstdint>
namespace forza::pause_camera {
bool Enabled();
// UI lifecycle on the main loop thread. A visible, validated native pause must
// pulse every tick; hides/releases cancel before invoking any native transition.
void Pulse(uint32_t screen,bool eligible);
void Cancel(uint32_t screen);
void World(uint32_t world);

// Scoped use at the two existing director matrix readers in vr_head_pose.cpp.
// No camera switch, FOV/projection change, car write or simulation update.
class ScopedView {
public:
  ScopedView(uint8_t* base,uint32_t director,bool presentation);
  ~ScopedView();
  ScopedView(const ScopedView&)=delete;
  ScopedView& operator=(const ScopedView&)=delete;
private:
  uint8_t* matrix_=nullptr;
  uint8_t original_[64]{},posed_[64]{};
};
}
