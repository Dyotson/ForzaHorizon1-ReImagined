#pragma once
#include "guest_addr.h"
#include <array>
#include <cstdint>
namespace forza::pause_camera {
// Observed native scene identity, not an owned view/texture or GPU-ready frame.
// A future studio producer must retain native references and qualify rendering
// before using it. No native function is called to obtain this observation.
struct NativeScene {
  uint32_t screen=0,world_car=0,core=0,camera=0,selection=0;
  uint64_t frame=0;
  std::array<float,16> view{};
};
#if FORZA_TU4 && defined(FORZA_NATIVE_PAUSE)
bool Enabled();
bool CompositionActive(uint32_t screen);
void ObserveSceneCamera(uint8_t* base,uint32_t camera);
bool ReadObservedScene(uint32_t screen,NativeScene& scene);
// UI lifecycle on the main loop thread. A visible, validated native pause must
// pulse every tick; hides/releases cancel before invoking any native transition.
// eligibility: enabled=1, free roam=2, offline=4, supported composition page=8.
void Pulse(uint32_t screen,bool eligible,uint32_t eligibility=0);
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
#else
inline bool Enabled() { return false; }
inline bool CompositionActive(uint32_t) { return false; }
inline void ObserveSceneCamera(uint8_t*,uint32_t) {}
inline bool ReadObservedScene(uint32_t,NativeScene&) { return false; }
inline void Pulse(uint32_t,bool,uint32_t=0) {}
inline void Cancel(uint32_t) {}
inline void World(uint32_t) {}
class ScopedView {
public:
  ScopedView(uint8_t*,uint32_t,bool) {}
};
#endif
}
