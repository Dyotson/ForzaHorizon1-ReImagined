#include "../src/platform/pause_fixed_pose_hold.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace forza::pause_camera;
int main(){
 const std::array<Vec3,4> hubs{{{-.8f,-.25f,1.4f},{.8f,-.25f,1.4f},{.8f,-.25f,-1.3f},{-.8f,-.25f,-1.3f}}};Anchor a;assert(WheelAnchor({123,1,-456},{0,0,1},{0,1,0},hubs,a));const Pose original{a.center-Vec3{0,0,5}+Vec3{0,1.5f,0},a.center,{0,1,0}};
 Transition tr;assert(tr.Begin(10,20,a,original));FixedPoseHold hold;std::array<float,16> first{};
 for(unsigned frame=0;frame<900;++frame){const auto sample=FixedPoseHold::Sample(true,tr,frame%2?.016:9.0);assert(sample.elapsed==1&&sample.background==0&&tr.Sample().elapsed==0);assert(WorldShotAllowed(sample));Pose pose;assert(Framing(sample,a,1.f,16.f/9,pose));std::array<float,16> matrix;assert(View(pose,matrix));assert(hold.Check(true,std::bit_cast<uint32_t>(1.f),16.f/9,matrix));if(!frame)first=matrix;for(size_t i=0;i<16;++i)assert(std::bit_cast<uint32_t>(matrix[i])==std::bit_cast<uint32_t>(first[i]));}
 assert(hold.active);auto changed=first;changed[0]=std::bit_cast<float>(std::bit_cast<uint32_t>(changed[0])+1);assert(!hold.Check(true,std::bit_cast<uint32_t>(1.f),16.f/9,changed));assert(!hold.Check(true,std::bit_cast<uint32_t>(1.f)+1,16.f/9,first));assert(!hold.Check(true,std::bit_cast<uint32_t>(1.f),2.f,first));
 assert(hold.Check(false,0,0,first)&&!hold.active);const auto normal=FixedPoseHold::Sample(false,tr,.1);assert(normal.elapsed==.1&&tr.Sample().elapsed==.1);tr.Cancel();hold.Reset();assert(!tr.Active()&&!hold.active);assert(tr.WheelHold().elapsed==0);
 assert(!FramingProjection(std::numeric_limits<float>::quiet_NaN(),16.f/9));puts("PASS diagnosticrearwheelhold900frames exactposebits/time/background0; noAdvance/noPan; projection/viewbitmismatchreject; defaultofforiginalAdvance; Cancelresets");
}
