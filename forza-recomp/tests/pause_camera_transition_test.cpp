#include "../src/platform/pause_camera_framing.h"
#include <cassert>
#include <limits>

using namespace forza::pause_camera;
static bool Near(Vec3 a,Vec3 b,float tolerance=.002f) { return Dot(a-b,a-b)<tolerance*tolerance; }
static Vec3 Project(const Pose& pose,Vec3 point,float fov,float aspect) {
  const auto f=Unit(pose.target-pose.eye),r=Unit(Cross(pose.up,f)),u=Cross(f,r);
  const auto relative=point-pose.eye;const float depth=Dot(relative,f);
  return {640*(1+Dot(relative,r)/(depth*std::tan(fov*.5f)*aspect)),
          360*(1-Dot(relative,u)/(depth*std::tan(fov*.5f))),depth};
}
static void Composition() {
  const std::array<Vec3,4> hubs{{{-.8f,-.25f,1.4f},{.8f,-.25f,1.4f},
                               {.8f,-.25f,-1.3f},{-.8f,-.25f,-1.3f}}};
  for(int degrees=0;degrees<360;degrees+=15)for(int fps:{30,60,120})
    for(float fov:{.6f,1.f,1.4f})for(float aspect:{1.f,16.f/9,2.39f}) {
      const float angle=float(degrees)*.01745329252f;
      const Vec3 forward{std::sin(angle),0,std::cos(angle)},up{0,1,0};
      const auto right=Cross(up,forward);Anchor car;
      assert(WheelAnchor({123,1,-456},forward,up,hubs,car));
      const Pose original{car.center-forward*5+up*1.5f,car.center,up};
      Transition transition;assert(transition.Begin(1,2,car,original));Pose framed;
      assert(Framing(transition.Sample(),car,fov,aspect,framed));
      assert(Near(framed.eye,original.eye)&&Near(framed.target,original.target));
      for(int tick=1;tick<=fps*11;++tick) {
        const auto frame=transition.Advance(1./fps);
        assert(Framing(frame,car,fov,aspect,framed));
        if(tick==fps) {
          const auto wheel=Project(framed,car.rear_wheel,fov,aspect);
          assert(std::abs(wheel.x-980)<.08f&&wheel.y>kCarTop&&wheel.y<kCarBottom&&wheel.z>0);
        }
        if(tick>=fps*3)for(int x:{-1,1})for(int z:{-1,1})for(float y:{car.low_y,car.high_y}) {
          const auto corner=car.center+right*(x*car.half_width)+forward*(z*car.half_length)+up*y;
          const auto screen=Project(framed,corner,fov,aspect);
          assert(screen.z>=.19f&&screen.x>=kCarLeft-.03f&&screen.x<=kCarRight+.03f&&
                 screen.y>=kCarTop-.03f&&screen.y<=kCarBottom+.03f);
        }
      }
      for(float invalid:{0.f,-1.f,3.f,std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        assert(!Framing(transition.Sample(),car,invalid,aspect,framed));
        if(invalid!=3.f)assert(!Framing(transition.Sample(),car,fov,invalid,framed));
      }
    }
}
int main() {
  Composition();
  const std::array<Vec3,4> hubs{{{-.8f,-.25f,1.4f},{.8f,-.25f,1.4f},
                               {.8f,-.25f,-1.3f},{-.8f,-.25f,-1.3f}}};
  Anchor car;
  assert(WheelAnchor({123,1,-456},{0,0,1},{0,1,0},hubs,car));
  assert(Near(car.rear_wheel,{123.8f,.75f,-457.3f}));
  auto invalid_hubs=hubs;invalid_hubs[0]=invalid_hubs[1];
  assert(!WheelAnchor({},{0,0,1},{0,1,0},invalid_hubs,car));
  assert(!WheelAnchor({},{0,0,1},{0,-1,0},hubs,car));
  const Pose original{{123,2.5f,-461},{123,1,-456},{0,1,0}};
  std::array<float,16> matrix;
  assert(View(original,matrix));
  Pose roundtrip;
  assert(PoseFromView(matrix,roundtrip));
  assert(Near(roundtrip.eye,original.eye));
  assert(Near(Unit(roundtrip.target-roundtrip.eye),Unit(original.target-original.eye)));
  matrix[0]=2;assert(!PoseFromView(matrix,roundtrip));
  matrix[0]=std::numeric_limits<float>::quiet_NaN();assert(!PoseFromView(matrix,roundtrip));
  assert(!View({{}, {}, {0,1,0}},matrix));

  Frame reference;
  for(int fps:{30,60,120,144,240}) {
    Transition transition;
    assert(!transition.Active());
    assert(transition.Begin(1,2,car,original));
    assert(Near(transition.Sample().pose.eye,original.eye));
    float background=0;
    for(int i=0;i<fps*10;++i) {
      const auto frame=transition.Advance(1.0/fps);
      assert(View(frame.pose,matrix));
      Pose pose;assert(PoseFromView(matrix,pose)); // orthonormal, invertible at every step
      assert(frame.background>=background&&frame.background<=1);
      background=frame.background;
      if(i==fps-1)assert(Near(frame.pose.target,car.rear_wheel));
    }
    const auto frame=transition.Sample();
    assert(frame.background==1);
    if(fps==30)reference=frame;
    else assert(Near(frame.pose.eye,reference.pose.eye));
    const auto before=frame.pose.eye;
    for(double invalid:{-1.0,1.0,std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()}) {
      assert(Near(transition.Advance(invalid).pose.eye,before));
    }
    // Interruptions and repeated pause must start with a new baseline, not a
    // partly completed old pan or fade. Resume cancels without an exit dolly.
    transition.Cancel();assert(!transition.Active());assert(transition.Sample().background==0);
    assert(transition.Begin(3,4,car,original));
    assert(transition.owner()==3&&transition.car()==4);
    assert(transition.Sample().elapsed==0&&Near(transition.Sample().pose.eye,original.eye));
    assert(!transition.Begin(3,4,{{},{0,1,0},{}},original));assert(!transition.Active());
    assert(!transition.Begin(0,4,car,original));
  }
  // Rotate the car through a full turn. No heading should change world-up,
  // handedness or the camera's distance, including the wrap at 2pi.
  for(int i=0;i<=360;++i) {
    const float angle=float(i)*.01745329252f;
    Transition transition;
    Anchor rotated;
    assert(WheelAnchor({}, {std::sin(angle),0,std::cos(angle)}, {0,1,0},hubs,rotated));
    assert(transition.Begin(1,2,rotated,original));
    for(int n=0;n<120;++n)assert(View(transition.Advance(1.0/30).pose,matrix));
  }
}
