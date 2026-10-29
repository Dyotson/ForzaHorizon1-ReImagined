#pragma once
#include "pause_camera_framing.h"
#include <bit>
namespace forza::pause_camera {
struct FixedPoseHold {
 bool active=false;uint32_t fov=0,aspect=0;std::array<float,16> view{};
 void Reset(){active=false;fov=aspect=0;view={};}
 static Frame Sample(bool requested,Transition& transition,double dt){return requested?transition.WheelHold():transition.Advance(dt);}
 bool Check(bool requested,uint32_t fov_bits,float a,const std::array<float,16>& m){if(!requested){Reset();return true;}const auto ab=std::bit_cast<uint32_t>(a);if(!active){active=true;fov=fov_bits;aspect=ab;view=m;return true;}if(fov!=fov_bits||aspect!=ab)return false;for(size_t i=0;i<16;++i)if(std::bit_cast<uint32_t>(view[i])!=std::bit_cast<uint32_t>(m[i]))return false;return true;}
};
}
