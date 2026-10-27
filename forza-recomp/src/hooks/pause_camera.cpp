// Authored camera-only experiment. Studio compositing is deliberately not
// connected until the car/background render boundary has been qualified.
#include "../platform/guest_addr.h"
#include "../platform/pause_camera.h"

#if FORZA_TU4 && defined(FORZA_NATIVE_PAUSE)
#include "../platform/guest_memory.h"
#include "../platform/pause_camera_transition.h"
#include "../platform/xr.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(forza_pause_camera, false, "Interface",
    "Experimental TU4 native-pause rear-quarter camera and slow pan (camera only; no studio background)");
REXCVAR_DECLARE(bool, vr_head_pose_test);

namespace forza::pause_camera {
namespace {
using Clock=std::chrono::steady_clock;
struct State {
  std::mutex mutex;
  uint32_t requested=0,world=0,director=0;
  Clock::time_point pulse{},world_time{},previous{},frame_time{};
  Transition transition;
  Anchor anchor{};
  std::array<float,16> view{};
  bool frame=false,blocked=false;
} state;

// Guard every page of untrusted/stale guest objects before copying. Physical
// guest addresses always go through GuestPtr's Windows mapping adjustment.
uint8_t* Memory(uint8_t* base,uint32_t address,size_t size,bool write=false) {
  if(!base||address<0x100000||uint64_t(address)+size>0xF0000000ull)return nullptr;
  auto* p=GuestPtr(base,address);
  for(size_t i=0;i<size;) {
    size_t region=0;rex::memory::PageAccess access;
    if(!rex::memory::QueryProtect(p+i,region,access)||
       (int(access)&(write?3:1))!=(write?3:1))return nullptr;
    const auto page=rex::memory::page_size();
    i+=std::min(size-i,page-(reinterpret_cast<uintptr_t>(p+i)%page));
  }
  return p;
}
bool Word(uint8_t* base,uint32_t address,uint32_t& out) {
  const auto* p=Memory(base,address,4);if(!p)return false;
  uint32_t raw;std::memcpy(&raw,p,4);out=std::byteswap(raw);return true;
}
bool Vector(uint8_t* base,uint32_t address,Vec3& out) {
  uint32_t x,y,z;
  if(!Word(base,address,x)||!Word(base,address+4,y)||!Word(base,address+8,z))return false;
  out={std::bit_cast<float>(x),std::bit_cast<float>(y),std::bit_cast<float>(z)};
  return Finite(out)&&Dot(out,out)<1e12f;
}
bool Camera(uint8_t* base,uint32_t& director,uint32_t& car,Anchor& anchor) {
  uint32_t root,holder,manager,first,last,active,table,first_director,second_director;
  if(!Word(base,0x833CAAE4,root)||!Word(base,root+4,holder)||!Word(base,holder+4,manager)||
     !Word(base,manager+244+8,first_director))return false;
  if(!director)director=first_director; // frustum reader: resolve only after gates
  if(!director||first_director!=director||
     !Word(base,manager+244+172+8,second_director)||second_director||
     !Word(base,director+20,active)||!Word(base,active,table)||
     (table!=FORZA_ADDR(0x820755AC)&&table!=FORZA_ADDR(0x8207562C))||
     !Word(base,manager+0x80,first)||!Word(base,manager+0x84,last)||
     last<first||last-first>256||(last-first)%4)return false;
  unsigned locals=0;
  for(uint32_t slot=first;slot<last;slot+=4) {
    uint32_t vehicle,kind,candidate,dynamics;
    if(!Word(base,slot,vehicle)||vehicle>0xEFFF0000||
       !Word(base,vehicle+0x2DD0,kind))return false;
    if(kind)continue;
    Vec3 up;
    if(++locals!=1||!Word(base,vehicle+0x2D90,candidate)||candidate<0x100A50||
       !Word(base,candidate,dynamics)||dynamics!=0x82003F0C||
       !Vector(base,candidate-0xA20,anchor.center)||
       !Vector(base,candidate-0xA40,anchor.forward)||
       !Vector(base,candidate-0xA50,up))return false;
    // TU4 CCamWheel::Update8290D098 reads virtual+318. Its implementation
    // 82DDFFF8 copies the car-space hub at +1350 + wheel*410. Read only;
    // no invoking/updating another camera, and no guest scratch allocation.
    std::array<Vec3,4> hubs;
    for(unsigned wheel=0;wheel<4;++wheel) {
      uint32_t radius_bits;
      if(!Vector(base,candidate+0x1350+wheel*0x410,hubs[wheel])||
         !Word(base,candidate+0x12A8+wheel*0x410,radius_bits))return false;
      const auto radius=std::bit_cast<float>(radius_bits);
      if(!std::isfinite(radius)||radius<.15f||radius>.8f)return false;
    }
    if(!WheelAnchor(anchor.center,anchor.forward,up,hubs,anchor))return false;
    car=candidate;
  }
  return locals==1;
}
void Reset(bool block=false) {
  state.transition.Cancel();state.director=0;state.frame=false;state.blocked=block;
}
bool Gates(uint8_t* base,Clock::time_point now) {
  if(!REXCVAR_GET(forza_pause_camera)||REXCVAR_GET(vr_head_pose_test)||
     xr::CockpitActive()||!state.requested||state.blocked||
     now-state.pulse>std::chrono::milliseconds(250)||
     now-state.world_time>std::chrono::milliseconds(250))return false;
  const auto* w=Memory(base,state.world,0x2c);if(!w||!w[0xc]||!w[0xd])return false;
  uint32_t raw;std::memcpy(&raw,w+0x28,4);
  const float scale=std::bit_cast<float>(std::byteswap(raw));
  return std::isfinite(scale)&&scale>=0; // rewind is never a camera context
}
}

// Observed only, after the original world update. No simulation control.
void World(uint32_t world) {
  if(!REXCVAR_GET(forza_pause_camera))return;
  std::lock_guard lock(state.mutex);state.world=world;state.world_time=Clock::now();
}
bool Enabled() { return REXCVAR_GET(forza_pause_camera); }
void Pulse(uint32_t screen,bool eligible) {
  std::lock_guard lock(state.mutex);
  if(!eligible||!screen||!REXCVAR_GET(forza_pause_camera)) {
    if(state.requested==screen||!screen){state.requested=0;Reset();}return;
  }
  const auto now=Clock::now();
  if(screen!=state.requested||now-state.pulse>std::chrono::milliseconds(250))Reset();
  state.requested=screen;state.pulse=now;
}
void Cancel(uint32_t screen) {
  std::lock_guard lock(state.mutex);
  if(!screen||screen==state.requested){state.requested=0;Reset();}
}

ScopedView::ScopedView(uint8_t* base,uint32_t director,bool presentation) {
  if(!REXCVAR_GET(forza_pause_camera))return;
  std::lock_guard lock(state.mutex);
  const auto now=Clock::now();
  if(!Gates(base,now)){Reset(state.blocked);return;}
  uint32_t car=0;Anchor anchor;
  if(!Camera(base,director,car,anchor)){Reset(true);return;}
  auto* matrix=Memory(base,director+240,64,true);if(!matrix){Reset(true);return;}
  if(presentation) {
    if(state.transition.Active()&&
       (state.director!=director||state.transition.car()!=car||
        Dot(anchor.center-state.anchor.center,anchor.center-state.anchor.center)>.01f||
        Dot(anchor.forward-state.anchor.forward,anchor.forward-state.anchor.forward)>.001f||
        Dot(anchor.rear_wheel-state.anchor.rear_wheel,anchor.rear_wheel-state.anchor.rear_wheel)>.01f)){
      Reset(true);return;
    }
    if(!state.transition.Active()) {
      std::array<float,16> original;
      for(unsigned i=0;i<16;++i) {
        uint32_t raw;std::memcpy(&raw,matrix+i*4,4);original[i]=std::bit_cast<float>(std::byteswap(raw));
      }
      Pose pose;
      if(!PoseFromView(original,pose)||Dot(pose.eye-anchor.center,pose.eye-anchor.center)>225||
         !state.transition.Begin(state.requested,car,anchor,pose)){Reset(true);return;}
      state.director=director;state.previous=now;state.anchor=anchor;
      REXLOG_INFO("Pause camera: camera-only experiment began, screen {:08X}, director {:08X}, car {:08X}; studio fade unbound",
                   state.requested,director,car);
    }
    const auto sample=state.transition.Advance(std::chrono::duration<double>(now-state.previous).count());
    state.previous=now;
    state.frame=View(sample.pose,state.view);state.frame_time=now;
  }
  if(!state.frame||state.director!=director||now-state.frame_time>std::chrono::milliseconds(250))return;
  matrix_=matrix;std::memcpy(original_,matrix,64);
  for(unsigned i=0;i<16;++i) {
    const uint32_t raw=std::byteswap(std::bit_cast<uint32_t>(state.view[i]));
    std::memcpy(posed_+4*i,&raw,4);
  }
  std::memcpy(matrix_,posed_,64);
}
ScopedView::~ScopedView() {
  // Do not undo a newer guest write made inside the native consumer.
  if(matrix_&&std::memcmp(matrix_,posed_,64)==0)std::memcpy(matrix_,original_,64);
}
} // namespace forza::pause_camera
#else
namespace forza::pause_camera {
bool Enabled() { return false; }
void World(uint32_t) {}
void Pulse(uint32_t,bool) {}
void Cancel(uint32_t) {}
ScopedView::ScopedView(uint8_t*,uint32_t,bool) {}
ScopedView::~ScopedView() {}
}
#endif
