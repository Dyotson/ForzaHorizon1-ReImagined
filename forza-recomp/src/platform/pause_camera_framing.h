#pragma once
#include "pause_camera_transition.h"

namespace forza::pause_camera {
// Shared 1280x720 composition contract. Controls occupy the left panel;
// wheel/car targets occupy the right region below the header and above footer.
inline constexpr float kMenuRight=672;
inline constexpr float kCarLeft=720,kCarRight=1240,kCarTop=168,kCarBottom=552;
inline constexpr float kTargetNdcX=(kCarLeft+kCarRight)/1280.f-1.f;
// Actual Garage-exterior captures show scenery crossing the car during the
// orbit. Until an owned scenery-free studio scene exists, release the authored
// view at the end of pullback instead of leaving that known failing pan active.
// This bounds the experiment; it is not a collision test for the earlier path.
inline bool WorldShotAllowed(const Frame& frame) {
  return std::isfinite(frame.elapsed)&&frame.elapsed>=0&&frame.elapsed<3.;
}
inline bool FramingProjection(float fov,float aspect) {
  return std::isfinite(fov)&&fov>.1f&&fov<3.f&&std::isfinite(aspect)&&aspect>.5f&&aspect<4.f;
}
inline bool Framing(const Frame& frame,const Anchor& anchor,float fov,float aspect,Pose& out) {
  if(!FramingProjection(fov,aspect)||!std::isfinite(frame.elapsed)||frame.elapsed<0)return false;
  const auto& pose=frame.pose;
  const auto delta=pose.target-pose.eye;const float distance=std::sqrt(Dot(delta,delta));
  const auto f=Unit(delta),r=Unit(Cross(pose.up,f)),u=Cross(f,r);
  if(!Finite(pose.eye)||!Finite(pose.target)||!Finite(pose.up)||distance<.01f||
     Dot(r,r)<.99f||!Finite(anchor.center)||!Finite(anchor.forward)||!Finite(anchor.up)||
     !std::isfinite(anchor.half_width)||!std::isfinite(anchor.half_length)||
     !std::isfinite(anchor.low_y)||!std::isfinite(anchor.high_y)||
     anchor.half_width<=0||anchor.half_width>4||anchor.half_length<=0||anchor.half_length>8||
     anchor.low_y>=anchor.high_y)return false;
  const float tan_v=std::tan(fov*.5f),tan_h=tan_v*aspect;
  // Translate in the camera's horizontal plane, retaining its original up
  // and direction. Re-aiming at the old target would center the car again.
  const float shift=kTargetNdcX*tan_h*Ease(frame.elapsed);
  float fitted=distance;
  const float pullback=Ease((frame.elapsed-1.4)/1.6);
  if(pullback>0) {
    const float left=(2*kCarLeft/1280-1)*tan_h,right=(2*kCarRight/1280-1)*tan_h;
    const float top=(1-2*kCarTop/720)*tan_v,bottom=(1-2*kCarBottom/720)*tan_v;
    const float left_div=shift-left,right_div=right-shift;
    if(left_div<=0||right_div<=0||top<=0||bottom>=0)return false;
    const auto car_right=Cross(anchor.up,anchor.forward);
    for(int x:{-1,1})for(int z:{-1,1})for(float y:{anchor.low_y,anchor.high_y}) {
      const auto relative=anchor.center+car_right*(x*anchor.half_width)+
          anchor.forward*(z*anchor.half_length)+anchor.up*y-pose.target;
      const float px=Dot(relative,r),py=Dot(relative,u),pz=Dot(relative,f);
      fitted=std::max({fitted,(left*pz-px)/left_div,(px-right*pz)/right_div,
          py/top-pz,py/bottom-pz,.2f-pz});
    }
    if(!std::isfinite(fitted)||fitted>40)return false;
  }
  const float d=distance+(fitted-distance)*pullback;
  const auto eye=pose.target-f*d-r*(shift*d);
  out={eye,eye+f*distance,pose.up};
  return Finite(out.eye)&&Finite(out.target);
}
} // namespace forza::pause_camera
