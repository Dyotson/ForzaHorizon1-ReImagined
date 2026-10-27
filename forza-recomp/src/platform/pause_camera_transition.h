#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace forza::pause_camera {
struct Vec3 {
  float x=0, y=0, z=0;
  Vec3 operator+(Vec3 b) const { return {x+b.x,y+b.y,z+b.z}; }
  Vec3 operator-(Vec3 b) const { return {x-b.x,y-b.y,z-b.z}; }
  Vec3 operator*(float t) const { return {x*t,y*t,z*t}; }
};
inline float Dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline bool Finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
inline Vec3 Unit(Vec3 a) { const float n=std::sqrt(Dot(a,a));return n>1e-5f?a*(1/n):Vec3{}; }
inline Vec3 Lerp(Vec3 a,Vec3 b,float t) { return a+(b-a)*t; }
inline float Ease(double t) {
  const double v=std::clamp(t,0.0,1.0);
  return float(std::clamp(v*v*v*(v*(v*6-15)+10),0.0,1.0));
}
struct Pose { Vec3 eye, target, up{0,1,0}; };
struct Anchor { Vec3 center, forward, rear_wheel; };
// CCarDynamics' wheel camera getter supplies four car-space hubs. Select by
// geometry rather than assuming its wheel-number convention. Unknown layouts
// (not two axles/two sides, or implausible scale) retain the original view.
inline bool WheelAnchor(Vec3 center,Vec3 forward,Vec3 up,
                        const std::array<Vec3,4>& hubs,Anchor& out) {
  if(!Finite(center)||!Finite(forward)||!Finite(up)||
     std::abs(Dot(forward,forward)-1)>.02f||std::abs(Dot(up,up)-1)>.02f||
     std::abs(Dot(forward,up))>.02f||up.y<.96f)return false;
  const auto right=Cross(up,forward);
  float rear=10,front=-10;
  for(const auto h:hubs) {
    if(!Finite(h)||std::abs(h.x)<.35f||std::abs(h.x)>1.8f||std::abs(h.y)>2||
       std::abs(h.z)>4)return false;
    rear=std::min(rear,h.z);front=std::max(front,h.z);
  }
  if(rear>-.3f||front<.3f||front-rear<.8f||front-rear>5.5f)return false;
  unsigned corners=0;
  Vec3 target;
  for(const auto h:hubs) {
    const bool back=std::abs(h.z-rear)<.25f;
    if(!back&&std::abs(h.z-front)>=.25f)return false;
    const unsigned corner=(back?2u:0u)+(h.x>0?1u:0u);
    if(corners&(1u<<corner))return false;
    corners|=1u<<corner;
    if(back&&h.x>0)target=center+right*h.x+up*h.y+forward*h.z;
  }
  if(corners!=15)return false;
  out={center,forward,target};return true;
}
struct Frame {
  Pose pose;
  // Separate output for a future verified car/background compositor. A caller
  // must not apply this to the whole image, which would fade the car and UI too.
  float background=0;
  double elapsed=0;
};

inline bool View(const Pose& pose,std::array<float,16>& m) {
  if(!Finite(pose.eye)||!Finite(pose.target)||!Finite(pose.up))return false;
  const auto f=Unit(pose.target-pose.eye),r=Unit(Cross(pose.up,f)),u=Cross(f,r);
  if(Dot(f,f)<.99f||Dot(r,r)<.99f)return false;
  m={r.x,u.x,f.x,0,r.y,u.y,f.y,0,r.z,u.z,f.z,0,
     -Dot(pose.eye,r),-Dot(pose.eye,u),-Dot(pose.eye,f),1};
  return true;
}
inline bool PoseFromView(const std::array<float,16>& m,Pose& p) {
  for(float f:m)if(!std::isfinite(f))return false;
  const Vec3 r{m[0],m[4],m[8]},u{m[1],m[5],m[9]},f{m[2],m[6],m[10]};
  if(std::abs(Dot(r,r)-1)>.02f||std::abs(Dot(u,u)-1)>.02f||
     std::abs(Dot(f,f)-1)>.02f||std::abs(Dot(r,u))>.02f||
     std::abs(Dot(r,f))>.02f||std::abs(Dot(u,f))>.02f||
     Dot(Cross(r,u),f)<.98f||std::abs(m[3])+std::abs(m[7])+std::abs(m[11])>.001f||
     std::abs(m[15]-1)>.001f)return false;
  p.eye=r*(-m[12])+u*(-m[13])+f*(-m[14]);p.target=p.eye+f*5;p.up=u;
  return Finite(p.eye);
}

// Owns only authored transition time and an immutable paused-car anchor. The
// adapter must cancel on resume, child screens, owner/car changes or lost gates.
// No inverse animation may run after simulation resumes.
class Transition {
public:
  bool Begin(uint32_t owner,uint32_t car,const Anchor& a,const Pose& original) {
    Cancel();
    std::array<float,16> check;
    if(!owner||!car||!Finite(a.center)||!Finite(a.forward)||
       !Finite(a.rear_wheel)||Dot(a.rear_wheel-a.center,a.rear_wheel-a.center)>25||
       Dot(a.forward,a.forward)<.8f||Dot(a.forward,a.forward)>1.2f||
       std::abs(a.forward.y)>.15f||!View(original,check))return false;
    owner_=owner;car_=car;anchor_=a;original_=original;return true;
  }
  void Cancel() { owner_=car_=0;time_=0; }
  bool Active() const { return owner_!=0; }
  uint32_t owner() const { return owner_; }
  uint32_t car() const { return car_; }
  Frame Advance(double dt) {
    // Freeze through long stalls; never fast-forward a wheel shot on return.
    if(std::isfinite(dt)&&dt>=0&&dt<=.25)time_+=dt;
    return Sample();
  }
  Frame Sample() const {
    if(!Active())return {};
    const Vec3 up{0,1,0};
    const auto f=Unit(Vec3{anchor_.forward.x,0,anchor_.forward.z});
    const auto r=Cross(up,f);
    // The adapter supplies a validated native rear hub; only the dolly offset
    // is authored. Collision clearance remains a runtime qualification gate.
    const auto wheel=anchor_.rear_wheel;
    const Pose close{wheel+r*1.65f-f*.6f+up*.35f,wheel,up};
    const float pan=float(std::sin(std::max(0.0,time_-3.0)*.08)*.25);
    const Pose wide{anchor_.center+r*(4.8f*std::cos(pan))-f*(2.3f+4.8f*std::sin(pan))+up*1.2f,
                    anchor_.center+up*.1f,up};
    Pose out;
    if(time_<1.0) {
      const float t=Ease(time_);
      out={Lerp(original_.eye,close.eye,t),Lerp(original_.target,close.target,t),Lerp(original_.up,up,t)};
    } else {
      const float t=Ease((time_-1.4)/1.6);
      out={Lerp(close.eye,wide.eye,t),Lerp(close.target,wide.target,t),up};
    }
    return {out,Ease((time_-.3)/1.2),time_};
  }
private:
  uint32_t owner_=0,car_=0;
  double time_=0;
  Anchor anchor_{};
  Pose original_{};
};
} // namespace forza::pause_camera
