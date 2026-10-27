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
REXCVAR_DEFINE_BOOL(forza_pause_camera_trace, false, "Debug",
    "Log pause-camera eligibility and rejection reasons once per consumer (maximum 64 records)");
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

enum class Site { Lifecycle, Presentation, Frustum };
// Called only with state.mutex held. Fixed process-wide budget and per-reason,
// per-consumer suppression keep unsupported contexts from producing frame spam.
void Trace(Site site,const char* reason,uint32_t detail=0,uint32_t value=0) {
  if(!REXCVAR_GET(forza_pause_camera_trace))return;
  struct Key { Site site;const char* reason; };
  static std::array<Key,64> seen{};
  static size_t count=0;
  for(size_t i=0;i<count;++i)
    if(seen[i].site==site&&std::strcmp(seen[i].reason,reason)==0)return;
  if(count==seen.size())return;
  seen[count++]={site,reason};
  const auto now=Clock::now();
  const auto age=[now](Clock::time_point time)->int64_t {
    return time==Clock::time_point{}?-1:
        std::chrono::duration_cast<std::chrono::milliseconds>(now-time).count();
  };
  REXLOG_INFO("Pause camera trace: site={} reason={} screen={:08X} world={:08X} director={:08X} pulse_ms={} world_ms={} blocked={} detail={:08X} value={:08X}",
      int(site),reason,state.requested,state.world,state.director,age(state.pulse),
      age(state.world_time),state.blocked,detail,value);
}
bool Reject(Site site,const char* reason,uint32_t detail=0,uint32_t value=0) {
  Trace(site,reason,detail,value);return false;
}

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
bool Camera(uint8_t* base,uint32_t& director,uint32_t& car,Anchor& anchor,Site site) {
  uint32_t root,holder,manager,first,last,active,table,first_director,second_director;
  if(!Word(base,0x833CAAE4,root)||!Word(base,root+4,holder)||!Word(base,holder+4,manager)||
     !Word(base,manager+244+8,first_director))return Reject(site,"camera-root-chain");
  if(!director)director=first_director; // frustum reader: resolve only after gates
  if(!director||first_director!=director)return Reject(site,"director-mismatch",director,first_director);
  if(!Word(base,manager+244+172+8,second_director))return Reject(site,"second-director-unreadable",manager);
  if(second_director)return Reject(site,"second-director-present",manager,second_director);
  if(!Word(base,director+20,active)||!Word(base,active,table))return Reject(site,"active-camera-unreadable",director);
  if(table!=FORZA_ADDR(0x820755AC)&&table!=FORZA_ADDR(0x8207562C))return Reject(site,"unsupported-camera",active,table);
  if(!Word(base,manager+0x80,first)||!Word(base,manager+0x84,last))return Reject(site,"car-list-unreadable",manager);
  if(last<first||last-first>256||(last-first)%4)return Reject(site,"car-list-range",first,last);
  unsigned locals=0;
  for(uint32_t slot=first;slot<last;slot+=4) {
    uint32_t vehicle,kind,candidate,dynamics;
    if(!Word(base,slot,vehicle)||vehicle>0xEFFF0000||
       !Word(base,vehicle+0x2DD0,kind))return Reject(site,"car-entry-unreadable",slot);
    if(kind)continue;
    Vec3 up;
    if(++locals!=1)return Reject(site,"multiple-local-cars",vehicle,locals);
    if(!Word(base,vehicle+0x2D90,candidate)||candidate<0x100A50||
       !Word(base,candidate,dynamics))return Reject(site,"dynamics-unreadable",vehicle);
    if(dynamics!=0x82003F0C)return Reject(site,"unsupported-dynamics",candidate,dynamics);
    if(!Vector(base,candidate-0xA20,anchor.center)||
       !Vector(base,candidate-0xA40,anchor.forward)||
       !Vector(base,candidate-0xA50,up))return Reject(site,"car-basis-invalid",candidate);
    // TU4 CCamWheel::Update8290D098 reads virtual+318. Its implementation
    // 82DDFFF8 copies the car-space hub at +1350 + wheel*410. Read only;
    // no invoking/updating another camera, and no guest scratch allocation.
    std::array<Vec3,4> hubs;
    for(unsigned wheel=0;wheel<4;++wheel) {
      uint32_t radius_bits;
      if(!Vector(base,candidate+0x1350+wheel*0x410,hubs[wheel])||
         !Word(base,candidate+0x12A8+wheel*0x410,radius_bits))return Reject(site,"wheel-unreadable",candidate,wheel);
      const auto radius=std::bit_cast<float>(radius_bits);
      // The native getter returned 0.1225 for the loaded Mustang in the
      // qualified trace. This unused sanity value has no proven metre-scale
      // minimum; require positive/finite data and retain the conservative cap.
      // Actual shot geometry still comes exclusively from validated hub data.
      if(!std::isfinite(radius)||radius<=0||radius>.8f)return Reject(site,"wheel-radius-invalid",wheel,radius_bits);
    }
    if(!WheelAnchor(anchor.center,anchor.forward,up,hubs,anchor))return Reject(site,"wheel-anchor-invalid",candidate);
    car=candidate;
  }
  return locals==1||Reject(site,"no-local-car",first,last);
}
void Reset(bool block=false) {
  state.transition.Cancel();state.director=0;state.frame=false;state.blocked=block;
}
bool Gates(uint8_t* base,Clock::time_point now,Site site) {
  if(!REXCVAR_GET(forza_pause_camera))return false;
  if(REXCVAR_GET(vr_head_pose_test)||xr::CockpitActive())return Reject(site,"vr-context");
  if(!state.requested)return Reject(site,"no-pause-request");
  if(state.blocked)return Reject(site,"latched-context-rejection");
  if(now-state.pulse>std::chrono::milliseconds(250))return Reject(site,"stale-pause-pulse");
  if(now-state.world_time>std::chrono::milliseconds(250))return Reject(site,"stale-world-pulse");
  const auto* w=Memory(base,state.world,0x2c);
  if(!w)return Reject(site,"world-unreadable");
  if(!w[0xc]||!w[0xd])return Reject(site,"world-not-paused-loaded",w[0xc],w[0xd]);
  uint32_t raw;std::memcpy(&raw,w+0x28,4);
  const float scale=std::bit_cast<float>(std::byteswap(raw));
  return (std::isfinite(scale)&&scale>=0)||Reject(site,"world-scale-invalid",std::byteswap(raw)); // rewind is never a camera context
}
}

// Observed only, after the original world update. No simulation control.
void World(uint32_t world) {
  if(!REXCVAR_GET(forza_pause_camera))return;
  std::lock_guard lock(state.mutex);state.world=world;state.world_time=Clock::now();
  Trace(Site::Lifecycle,"world-observed",world);
}
bool Enabled() { return REXCVAR_GET(forza_pause_camera); }
void Pulse(uint32_t screen,bool eligible,uint32_t eligibility) {
  std::lock_guard lock(state.mutex);
  if(REXCVAR_GET(forza_pause_camera))Trace(Site::Lifecycle,eligible?"pause-eligible":"pause-ineligible",screen,eligibility);
  if(!eligible||!screen||!REXCVAR_GET(forza_pause_camera)) {
    if(state.requested==screen||!screen){state.requested=0;Reset();}return;
  }
  const auto now=Clock::now();
  if(screen!=state.requested||now-state.pulse>std::chrono::milliseconds(250))Reset();
  state.requested=screen;state.pulse=now;
}
void Cancel(uint32_t screen) {
  std::lock_guard lock(state.mutex);
  if(REXCVAR_GET(forza_pause_camera))Trace(Site::Lifecycle,"cancel",screen);
  if(!screen||screen==state.requested){state.requested=0;Reset();}
}

ScopedView::ScopedView(uint8_t* base,uint32_t director,bool presentation) {
  if(!REXCVAR_GET(forza_pause_camera))return;
  std::lock_guard lock(state.mutex);
  const auto now=Clock::now();
  const Site site=presentation?Site::Presentation:Site::Frustum;
  Trace(site,"consumer-entered",director);
  if(state.requested)Trace(site,"pause-consumer-entered",director);
  if(!Gates(base,now,site)){Reset(state.blocked);return;}
  uint32_t car=0;Anchor anchor;
  if(!Camera(base,director,car,anchor,site)){Reset(true);return;}
  auto* matrix=Memory(base,director+240,64,true);if(!matrix){Trace(site,"matrix-unwritable",director);Reset(true);return;}
  if(presentation) {
    if(state.transition.Active()&&
       (state.director!=director||state.transition.car()!=car||
        Dot(anchor.center-state.anchor.center,anchor.center-state.anchor.center)>.01f||
        Dot(anchor.forward-state.anchor.forward,anchor.forward-state.anchor.forward)>.001f||
        Dot(anchor.rear_wheel-state.anchor.rear_wheel,anchor.rear_wheel-state.anchor.rear_wheel)>.01f)){
      Trace(site,"active-context-changed",director,car);Reset(true);return;
    }
    if(!state.transition.Active()) {
      std::array<float,16> original;
      for(unsigned i=0;i<16;++i) {
        uint32_t raw;std::memcpy(&raw,matrix+i*4,4);original[i]=std::bit_cast<float>(std::byteswap(raw));
      }
      Pose pose;
      if(!PoseFromView(original,pose)){Trace(site,"view-pose-invalid",director);Reset(true);return;}
      if(Dot(pose.eye-anchor.center,pose.eye-anchor.center)>225){Trace(site,"view-too-distant",director,car);Reset(true);return;}
      if(!state.transition.Begin(state.requested,car,anchor,pose)){Trace(site,"transition-begin-rejected",director,car);Reset(true);return;}
      state.director=director;state.previous=now;state.anchor=anchor;
      REXLOG_INFO("Pause camera: camera-only experiment began, screen {:08X}, director {:08X}, car {:08X}; studio fade unbound",
                   state.requested,director,car);
    }
    const auto sample=state.transition.Advance(std::chrono::duration<double>(now-state.previous).count());
    state.previous=now;
    state.frame=View(sample.pose,state.view);state.frame_time=now;
    if(!state.frame)Trace(site,"sample-view-invalid");
  }
  if(!state.frame||state.director!=director||now-state.frame_time>std::chrono::milliseconds(250)){
    Trace(site,"no-current-camera-frame",director);return;
  }
  Trace(site,"matrix-applied",director);
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
#endif
